/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <cmath>
#include <list>
#include <string>
#include <vector>

#include "Creature.h"
#include "CreatureAI.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellAuras.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"

// The Halls of Stone driver — the imperative half of map 599's clear.
//
// TWO hooks, and only the second is a controller in the Black Morass sense.
// 22 garrisons the hold point for the Tribunal's fixed 300 seconds and owns the
// ONE recovery path this dungeon needs; 23 (HosDriveWave) re-decides from live
// world state every tick while adds are up. The DECLARATIVE half — the event
// rows, the wave predicate and the roster patch — stays in
// Data/Events/HallsOfStoneEvents.cpp, per the one-file-per-dungeon rule in
// DungeonEventTables.h, and the numbers both halves must agree on live in
// namespace DcHallsOfStone in that header.
//
// WHAT MAKES THIS ENCOUNTER DIFFERENT FROM THE OTHER TWO WAVE DUNGEONS, and
// where the rules below come from:
//
//   * THE CLOCK IS FIXED AND NOTHING SHORTENS IT. The Violet Hold advances when
//     you kill the keeper; the Black Morass advances when you kill the rift
//     lord. The Tribunal of Ages advances at 100s, 200s and 300s and at no other
//     time. Killing every add instantly buys nothing but a quieter next minute.
//     So there is no "off-switch" rung here, no target whose death is progress,
//     and the whole job is POSITIONAL: be between the adds and Brann for five
//     minutes without dying and without letting him die.
//
//   * THE THING BEING DEFENDED IS A FRIENDLY NPC WITH REGENERATION OFF AND NO
//     IMMUNITY, and his death is the encounter's ONLY fail condition
//     (JustDied -> ResetEvent, boss state 2 back to NOT_STARTED, despawn, and he
//     returns to his DB spawn 200yd away offering menu 9669 again). Every add is
//     Taunt-wired to him — each carries Taunt 51774 on respawn in smart_scripts,
//     and spell_taunt_brann makes the hit unit cast 51775 back — so "the adds are
//     coming for Brann" is not an inference from pathing, it is the script's
//     stated intent. Hence rule 2: PREFER THE ADD CLOSEST TO BRANN, not the one
//     closest to the tank. That is the single rule that makes the tank's job here
//     the right job.
//
//   * NO PULL IS EVER REQUIRED. JustSummoned calls SetInCombatWithZone() on every
//     add, so each one is in combat with the whole party the instant it exists.
//     The force-pull at rung 0 is therefore much narrower in purpose than the
//     Violet Hold's: it exists only for an add that has lost its victim and gone
//     idle, not to start fights.
//
//   * THE HEADS CANNOT BE TOUCHED. Kaddrak, Marnak and Abedneum are faction 114,
//     unit_flags 33554436 (NOT_SELECTABLE | 0x4), NullCreatureAI, hovering ~14yd
//     up. They are pure spell emitters. AttackersValue::IsPossibleTarget already
//     rejects NOT_SELECTABLE, so the clear's pickers cannot select them — but a
//     driver that went looking for "the source of the damage" would burn ticks
//     forever. Nothing below ever targets them; they are read for EXISTENCE only,
//     as the "is the Tribunal running" probe.
//
//   * TWO OF THE THREE HAZARDS FOLLOW YOU. Searing Gaze spawns its trigger AT a
//     random player's exact position and Dark Matter's chaser MOVES to a random
//     player before detonating, so no hold point avoids either. They are handled
//     as DcHazardEmitter rows (the party's own vacate machinery walks bots out of
//     them) and the driver deliberately does NOT re-implement that: a driver that
//     also dodged would fight the vacate action for the tick. Glare of the
//     Tribunal is a single-target beam every 1.5s on a random player within 100yd
//     with no radius at all — there is nowhere to stand, and it is the healer's
//     problem, not the driver's.

namespace
{
    using namespace DcHallsOfStone;

    // --- travel tuning -----------------------------------------------------

    // Beyond this a bare MovePoint is not trusted to deliver: the engine
    // PathGenerator caps a generated path at 74 polys / 74 points and truncates
    // SILENTLY past that — the bot simply stands still with no failure to observe
    // at the call site. Most hops inside this arena are short (the furthest add
    // spawn is 58.5yd from the hold point), but the recovery walk back to Brann's
    // DB spawn is 200yd, so the long-haul path has to exist.
    constexpr float HOS_LONG_HAUL = 30.0f;

    // "Is the glide in flight still aimed at the right place?" — NOT the arrival
    // leash, which is far tighter. A live add is a moving target, so comparing
    // against an arrival leash would cancel and re-path every tick. A CEILING
    // rather than a constant, scaled to half the trip with a 2yd floor, because
    // this dungeon has hops shorter than the epsilon itself: the walk from the
    // hold point to Brann is 25yd and the walk to the lore stop is 3.7yd, and a
    // flat 12yd epsilon would read "gliding there already" and issue nothing
    // forever (the Violet Hold Sinclari stall, in a new costume).
    constexpr float HOS_REPATH_EPSILON = 12.0f;

    float HosRepathEpsilon(float dist)
    {
        return std::min(HOS_REPATH_EPSILON, std::max(2.0f, dist * 0.5f));
    }

    // Floor between two spline issues for the SAME destination. Out of combat the
    // escort-generator check in HosTravelTo is enough on its own; IN COMBAT it is
    // not, because the stock combat engine layers MoveChase back over the escort
    // slot whenever it wins a tick, and this encounter is 250 seconds of unbroken
    // combat — without a time floor the driver would rebuild and re-issue a route
    // every single tick for four minutes.
    constexpr uint32 HOS_REISSUE_MS = 1500;

    // --- DELIVERY BANDS: where the driver stops steering -------------------
    // The driver's job is DELIVERY to the right part of the arena, not the last
    // few yards. Inside these bands it stands down and hands the tick back, and
    // the stock combat engine's MoveChase closes the rest while the tank fights.
    //
    // This is the Black Morass lesson and it is the most expensive mistake
    // available here. Engine::DoNextAction runs exactly ONE action per tick and
    // this driver sits ABOVE the stock combat movers, so a hook that reports
    // Running every tick leaves the tank standing in melee with no rotation and no
    // threat while the DPS pull aggro. And a tight arrival leash re-triggers
    // travel over a couple of yards, so a tank at melee range would break off,
    // walk two yards and re-enter combat, forever.
    constexpr float HOS_DELIVERED_TARGET = 25.0f;

    // Band for the position the driver PARKS on rather than fights at: the hold
    // point. Much tighter than the target band because there is no fight in
    // progress there and the whole value of the hold point is being ON the
    // intercept line — a 25yd "delivered" would leave the party a third of the way
    // to the add spawns. It cannot cause the walk-two-yards-and-re-engage thrash
    // the wide band exists to prevent, because rule 3 outranks this rung the
    // instant anything hostile is within reach.
    constexpr float HOS_DELIVERED_HOLD = 8.0f;

    // How close the driver walks the tank when it is OUT OF COMBAT and has already
    // decided to fight something. The delivery bands hand the last stretch to
    // MoveChase, which is right while the tank is fighting and wrong when it is
    // not: an unaggroed tank is on the NON-combat engine, where there is no
    // MoveChase to hand anything to. The arrival band is deliberately WIDER than
    // the leash it travels with, so there is no ring in which the travel call
    // declines to re-issue ("already there") while the arrival test still reads
    // "too far" and the tank holds the tick standing in it.
    constexpr float HOS_ENGAGE_CLOSE       = 4.0f;
    constexpr float HOS_ENGAGE_CLOSE_LEASH = 2.0f;

    // --- the wave rules ----------------------------------------------------

    // Force-pull radius, measured from the BOT. Deliberately NOT arena-wide.
    // Unlike the Violet Hold this is not how fights START here — every add is
    // SetInCombatWithZone()'d on summon and is already in combat with the whole
    // party. It is a repair for the one state that strands one: an add whose
    // victim died or evaded, standing idle where it last swung. 30yd is
    // "everything we are standing among" and falls well short of the spawn points
    // 43yd up the ramp, so it can never drag the party into the next wave early.
    constexpr float HOS_PULL_RADIUS = 30.0f;

    // Rule 3's band — "something is already on top of us, finish it before
    // moving". Deliberately the SAME number as the force-pull radius: everything
    // the driver drags into combat at rung 0 is something it then has to stand
    // and fight, and two different numbers would give the party a ring it pulls
    // from and refuses to fight in.
    constexpr float HOS_ENGAGE_BAND = HOS_PULL_RADIUS;

    // How far past the hold point the driver will chase an add toward Brann.
    //
    // THE LEASH IS ANCHORED ON BRANN, NOT ON THE TANK, and that is the whole
    // point of it. The failure this prevents is not "the tank wanders" — it is
    // "the tank follows an add UP THE RAMP toward the spawn points and leaves the
    // next batch a clear 83yd walk to a passive NPC with regeneration off". The
    // hold point is 25yd from Brann and the spawn points are 83yd from him, so a
    // 40yd leash lets the party meet an add well forward of the camp while still
    // being closer to Brann than any spawn is by a factor of two.
    constexpr float HOS_BRANN_LEASH = 40.0f;

    // --- travel ------------------------------------------------------------

    // PER-BOT throttled travel log, not a file-scope static: with several
    // instances running concurrently a global makes every consecutive line come
    // from a DIFFERENT bot, which makes the one measurement that matters ("is
    // THIS bot's distance falling tick over tick?") impossible to read.
    void HosTravelLog(Player* bot, char const* what, float dist, std::string const& detail)
    {
        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || DcRun::Of(botAI).Throttled(DcThrottle::HosTravelLog, 3000))
            return;
        LOG_DEBUG("playerbots.dungeonclear",
                  "DungeonClear: Halls of Stone — {} travel: {} (dist {:.1f}yd){}{}",
                  bot->GetName(), what, dist, detail.empty() ? "" : " — ", detail);
    }

    // Travel to an arena point, long-haul aware and safe to call every tick while
    // IN COMBAT. Structurally the Violet Hold's VhTravelTo, because the constraint
    // is the same one: the driver owns the tick precisely so it CAN take the bot
    // off its current target and reposition it, and HOS_REISSUE_MS is what keeps
    // that from becoming spline spam.
    //
    // NOTE what is deliberately NOT here: an `if (bot->isMoving()) return;` guard.
    // In combat the bot is essentially always moving under MoveChase, so such a
    // guard would make the driver a no-op for the entire encounter.
    void HosTravelTo(Player* bot, float x, float y, float z, float leash)
    {
        if (!bot)
            return;

        float const dist = bot->GetExactDist(x, y, z);
        if (dist <= leash)
            return;  // arrived

        float const epsilon = HosRepathEpsilon(dist);

        // An escort glide already in flight owns the bot. Let it finish if it is
        // headed here; drop it if it is stale — a route to the add we were chasing
        // ten seconds ago would otherwise ride all the way out before the bot could
        // react to the one now standing over Brann.
        MotionMaster* mm = bot->GetMotionMaster();
        float dx, dy, dz;
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
            mm->GetDestination(dx, dy, dz))
        {
            if (std::sqrt((dx - x) * (dx - x) + (dy - y) * (dy - y) + (dz - z) * (dz - z)) <=
                epsilon)
                return;  // gliding here already
            DcMovement::ResolveEscortConflict(bot);
        }

        // Same-destination re-issue floor: the check above cannot see a glide the
        // combat engine has since layered MoveChase over.
        if (PlayerbotAI* const issuer = GET_PLAYERBOT_AI(bot))
            if (DcRun::Of(issuer).ThrottledIssue(DcThrottle::HosTravelIssue, x, y, z,
                                                 epsilon, HOS_REISSUE_MS))
                return;

        if (dist > HOS_LONG_HAUL)
        {
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                ChunkedPathfinder::Result const path = LongRangePathfinder::Build(bot, x, y, z);
                HosTravelLog(bot,
                             path.reachable
                                 ? (path.segments.empty() ? "route reachable but EMPTY"
                                                          : "route ok -> issuing spline")
                                 : "route UNREACHABLE",
                             dist, path.failureReason);
                if (path.reachable && !path.segments.empty())
                {
                    // Element 0 is the live position — the escort path[0]=start
                    // convention SplinePath expects.
                    Movement::PointsArray points;
                    points.push_back(G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ()));
                    for (PathSegment const& seg : path.segments)
                    {
                        // A jump leg cannot be expressed as a ground spline. The
                        // whole Brann corridor is one continuous walkable descent
                        // (probed: z 208.4 at his spawn down to 203.9 at the
                        // console, no break), so this should never fire; deliver
                        // what we have if it does.
                        if (seg.jumpDown || seg.jumpGap)
                            break;
                        for (G3D::Vector3 const& p : seg.polyline)
                        {
                            if (points.size() >= DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS)
                                break;
                            points.push_back(p);
                        }
                    }
                    bool const issued = DcMovement::SplinePath(botAI, points);
                    HosTravelLog(bot, issued ? "spline ISSUED" : "spline REFUSED (paused/<2pts)",
                                 dist, std::to_string(points.size()) + " pts");
                    if (issued)
                        return;
                }
            }
            // No navmesh route: fall through. MovePoint will not deliver at this
            // range either, but the force-pull remains the backstop once mobs are
            // actually next to us.
        }

        bot->GetMotionMaster()->MovePoint(0, x, y, z,
                                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);
    }

    // Travel to a CREATURE, snapping the aim point onto the navmesh first.
    //
    // This matters more on map 599 than the flat arena floor suggests. The columns
    // under the Tribunal bowl carry a PHANTOM SURFACE at z ~ -142 and those under
    // the eastern half one at z ~ 0.16 (599 is in the flat-grid-height family,
    // ac-map601-flat-gridheight-zero), and the three heads hover 12-20yd above the
    // floor. Feeding a raw position to LongRangePathfinder asks it for a route to
    // a point that may have no polygon under it at all — the shape that resolves
    // to z 0.0 and sinks a bot through the world. NavmeshSnap's vertical extent is
    // a FIXED 10yd (dc-boss-anchor-snap-vertical-extent), which covers every add's
    // drift over this floor; if the snap misses, the raw position is still the
    // best guess available and is used unchanged.
    void HosTravelToCreature(Player* bot, Creature* c, float leash)
    {
        if (!bot || !c)
            return;
        float x = c->GetPositionX(), y = c->GetPositionY(), z = c->GetPositionZ();
        NavmeshSnap::Result const snap = NavmeshSnap::Snap(bot, x, y, z, /*maxRadius*/ 12.0f);
        if (snap.ok)
        {
            x = snap.x;
            y = snap.y;
            z = snap.z;
        }
        HosTravelTo(bot, x, y, z, leash);
    }

    // Back to the hold point — the intercept line between the three add spawns and
    // Brann. Shared by hook 22's garrison and rule 5 of the wave driver, which are
    // the same act seen from either side of the wave predicate. If the two rungs
    // disagreed about where "wait" is, every gap between waves would tug the party
    // a few yards toward the other one's answer.
    void HosHoldPoint(Player* bot)
    {
        HosTravelTo(bot, HOLD_X, HOLD_Y, HOLD_Z, HOS_DELIVERED_HOLD);
    }

    // ARRIVED: kill the delivery glide.
    //
    // The events set StepsOwnMovement, which strips the driving action's per-tick
    // ResolveEscortConflict — that hold was what cancelled the driver's own spline
    // the tick after it was issued. Taking that responsibility means taking the
    // other half too: NOTHING else stops the glide, so deciding "delivered" and
    // simply yielding leaves the spline in flight and it carries the tank on to
    // wherever it was originally aimed. ResolveEscortConflict only acts while an
    // ESCORT glide is the active generator, so this is a no-op once stopped and
    // never perturbs the MoveChase that closes the last few yards.
    void HosArrive(Player* bot)
    {
        DcMovement::ResolveEscortConflict(bot);
    }

    // Delivered at a destination: keep the tick or hand it over?
    //
    // FOR THE WAVE DRIVER ONLY (hook 23). Done here means "nothing to steer this
    // tick, take it back" — which is safe only because hook 23 drives a
    // REPEATABLE CONDITIONAL event, where a completed step latches nothing and
    // re-fires the next tick its predicate holds. Hook 22 is the Custom step of an
    // ANCHORED event, where RunStep maps Done straight to "this step is finished,
    // advance the event", so it must never call this. See rule 2 of
    // DriveHallsOfStoneTribunal.
    //
    // IN COMBAT, hand it over — that is the whole reason the yield exists. The
    // stock combat engine needs the tick to pick a target, swing, cast and hold
    // threat, and it only gets one action per tick.
    //
    // OUT OF COMBAT, keep it. The rung below is hook 22's garrison, and yielding
    // to it while the party is standing on the hold point waiting for the next
    // wave is harmless only because they agree; but yielding while standing
    // anywhere else hands the tick to a walk. Holding here is what pins the party
    // where the driver put it. Nothing is lost: out of combat there is no rotation
    // to run.
    ObjectiveArriveResult HosHold(Player* bot)
    {
        return bot->IsInCombat() ? ObjectiveArriveResult::Done
                                 : ObjectiveArriveResult::Running;
    }

    // --- selection ---------------------------------------------------------

    // Brann, by entry, anywhere in the arena. Never cached as a GUID: the script
    // despawns and respawns him with a DIFFERENT GUID between the Tribunal and the
    // door ("Sniff reveals different GUID, same entry"), and a driver holding the
    // old one would steer at a corpse pointer for the rest of the run.
    Creature* HosFindBrann(Player* bot, float radius)
    {
        return bot->FindNearestCreature(NPC_BRANN, radius, /*alive*/ true);
    }

    // True for a live Tribunal wave add. Used to decide whether the tank's CURRENT
    // victim is still the right thing to be hitting — `bot->GetVictim() != nullptr`
    // reads "has a victim" as "is fighting the right thing", and a tank holding a
    // stale victim never commits to the add standing over Brann.
    bool HosIsWaveAdd(Unit const* u)
    {
        if (!u || !u->IsAlive())
            return false;
        uint32 const entry = u->GetEntry();
        for (uint32 e : HallsOfStoneWaveEntries())
            if (entry == e)
                return true;
        return false;
    }

    // THE SELECTION RULE THIS ENCOUNTER TURNS ON: the add CLOSEST TO BRANN, not
    // the one closest to the tank.
    //
    // Every add is Taunt-wired to him (51774 on respawn in smart_scripts,
    // spell_taunt_brann bouncing 51775 back), so the population is always walking
    // toward one point, and the only thing the party can lose is that point. A
    // nearest-to-tank picker inverts the priority exactly when it matters: with one
    // add in the tank's face and one already swinging at Brann, it picks the former
    // and the encounter fails while the party is winning its fight.
    //
    // Bounded by HOS_BRANN_LEASH so this can never become a chase up the ramp
    // toward the spawn points — see that constant. Falls back to the hold point's
    // own position if Brann cannot be found (he is briefly absent across a
    // death/respawn), which keeps the leash meaningful rather than unbounded.
    Creature* HosAddNearestBrann(Player* bot, Creature* brann)
    {
        float const ax = brann ? brann->GetPositionX() : CONSOLE_X;
        float const ay = brann ? brann->GetPositionY() : CONSOLE_Y;

        std::list<Creature*> adds;
        bot->GetCreatureListWithEntryInGrid(adds, HallsOfStoneWaveEntries(), ARENA_SCAN);

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* c : adds)
        {
            if (!c || !c->IsAlive())
                continue;
            // Never chase past the leash, measured from BRANN — an add running
            // back up its own ramp is not a threat to him and is not worth the
            // party's position.
            float const toBrann = c->GetExactDist2d(ax, ay);
            if (toBrann > HOS_BRANN_LEASH)
                continue;
            if (!best || toBrann < bestDist)
            {
                best = c;
                bestDist = toBrann;
            }
        }
        return best;
    }

    // The nearest live wave add to the BOT within `radius` — rule 3's "something is
    // already on top of us" probe, and the only place proximity to the tank is the
    // right question.
    Creature* HosNearestAdd(Player* bot, float radius)
    {
        std::list<Creature*> adds;
        bot->GetCreatureListWithEntryInGrid(adds, HallsOfStoneWaveEntries(), radius);

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* c : adds)
        {
            if (!c || !c->IsAlive())
                continue;
            float const d = bot->GetExactDist2d(c);
            if (!best || d < bestDist)
            {
                best = c;
                bestDist = d;
            }
        }
        return best;
    }

    // Is the Tribunal running right now? Existence of any of the three heads.
    // They are summoned by InitializeEvent() and despawned together by
    // EndTribunalFight() and by ResetEvent() (Brann's death), so this is exact in
    // both directions — and it is the only probe that reads true during the quiet
    // first 52 seconds, before the first add spawns.
    bool HosHeadsUp(Player* bot)
    {
        for (uint32 head : HallsOfStoneHeadEntries())
            if (bot->FindNearestCreature(head, ARENA_SCAN, /*alive*/ true))
                return true;
        return false;
    }

    // --- engagement --------------------------------------------------------

    // Force a stranded add back into combat with the bot.
    //
    // MUCH NARROWER IN PURPOSE than its Violet Hold counterpart, and the guard is
    // the difference. Every add here arrives SetInCombatWithZone()'d, so this is
    // never how a fight starts; the `IsInCombat()` early-out means it only ever
    // fires for one that has LOST its combat — its victim died, or it evaded — and
    // is now standing idle between the party and Brann where the stock pickers may
    // or may not notice it.
    bool HosForcePull(Player* bot, Creature* c)
    {
        if (!c || !c->IsAlive() || c->IsInCombat())
            return false;
        // If it parked itself in a reset/home idle, clear that before the pull so
        // the AI does not immediately settle back into it.
        if (c->IsInEvadeMode())
            c->ClearUnitState(UNIT_STATE_EVADE);
        c->EngageWithTarget(bot);
        if (c->AI())
            c->AI()->AttackStart(bot);
        return true;
    }

    // Point the TANK at `target` and start swinging.
    //
    // HosForcePull is the other half of a pull and is not a substitute for this
    // one: EngageWithTarget/AttackStart make the CREATURE attack the BOT, and
    // nothing in that direction ever gives the bot a victim — a player does not
    // auto-retaliate. Normally Player::Attack comes from stock combat's own
    // targeting, but the driver has to be sure the tank commits to the add nearest
    // BRANN specifically, and to commit on the tick it decides rather than whenever
    // stock targeting next re-picks.
    //
    // Idempotent by the GetVictim() guard, so calling it every tick never fights
    // stock combat's target management once the tank is already on the right mob.
    bool HosEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
    {
        if (!bot || !target || !target->IsAlive())
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        bool const alreadyOnIt = bot->GetVictim() == target;
        if (!alreadyOnIt)
        {
            bot->SetSelection(target->GetGUID());
            if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
                ServerFacade::instance().SetFacingTo(bot, target);
            if (context)
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
            bot->Attack(target, botAI->IsMelee(bot));
        }

        // FLIP THE BOT ONTO THE COMBAT ENGINE. Engine transitions in
        // mod-playerbots are ACTION-DRIVEN, not derived from bot->IsInCombat():
        // Player::Attack alone just gives the bot a victim and the CORE then drives
        // auto-attack swings with no AI involvement, so a bot that is
        // force-attacked without the flip sits on the NON-combat engine, where no
        // class rotation exists, and melees the mob down one white hit at a time
        // with no threat abilities. Deliberately NOT inside the !alreadyOnIt
        // branch: once a bot has been left attacking on the wrong engine
        // GetVictim() already equals the target, so gating the flip on the target
        // CHANGE would latch the broken state permanently. ChangeEngine no-ops when
        // the engine already matches.
        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
        return !alreadyOnIt;
    }

    // --- hook 22: DriveHallsOfStoneTribunal --------------------------------
    //
    // The Custom step of the persistent "The Tribunal of Ages" objective. Three
    // jobs, re-decided per tick and NEVER latched, so a Drive restart cannot
    // double-fire anything:
    //
    //   1. DONE the moment bit 2 is set (or boss state 2 reads DONE). Completion
    //      rides the COMPLETED-ENCOUNTER MASK, not GetData: GetData(2) stays 0
    //      through the whole 300s fight and only starts toggling SPECIAL/DONE
    //      during the 256s of post-fight lore, so a data gate here would complete
    //      the objective at an arbitrary point in a cutscene. EndTribunalFight
    //      casts 59046, ObjectMgr stamps SPELL_ATTR0_CU_ENCOUNTER_REWARD on every
    //      cast-spell credit spell, Spell::finish calls Map::UpdateEncounterState,
    //      and bit 2 goes up. That is the same mask NextDungeonBossValue already
    //      reads for the other three.
    //
    //   2. RUNNING while it is in progress — garrison the hold point. In practice
    //      this rung only gets ticks in the gaps: from t ~ 52s the party is in
    //      continuous combat and event 4's rung preempts this one. That is by
    //      design; this is the between-waves half of the same position rule 5 of
    //      the wave driver holds.
    //
    //   3. RECOVERY, and this is the one genuinely intricate piece in this file.
    //      If the Tribunal reads NOT_STARTED with no heads up, BRANN IS DEAD and
    //      has respawned at his DB spawn 200yd away offering menu 9669 — the whole
    //      escort has to run again. Event 1 cannot do it: it latched Done the
    //      moment Kaddrak first appeared, minutes ago, and a Persistent anchored
    //      event never rewinds. So this hook owns it, mirroring the Violet Hold's
    //      hook 16 ("if NOT_STARTED, re-run the start logic"). The cost is a second
    //      ~2-minute escort, which is the difference between a recoverable run and
    //      a dead one.
    //
    //      It re-drives the gossip DIRECTLY rather than trying to re-arm the escort
    //      step, because it is the same act the escort's own resume branch performs
    //      and it is idempotent by construction: SelectGossip is a no-op while the
    //      menu is not populated, Brann drops UNIT_NPC_FLAG_GOSSIP the instant the
    //      select lands, and every path here re-reads instance state next tick.
    //
    // THE HOOK MUST YIELD THE TICK whenever it has nothing to steer. This is the
    // Black Morass trap: a driver that reports Running every tick starves the
    // combat engine and the tank stands in melee with no rotation. Rule 2's
    // garrison therefore ends in HosHold (Done in combat, Running out of it), never
    // in a bare Running.
    ObjectiveArriveResult DriveHallsOfStoneTribunal(Player* bot, AiObjectContext* /*context*/,
                                                    DungeonBossInfo const& /*info*/)
    {
        if (!bot || !bot->GetMap())
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // --- 1. finished ---------------------------------------------------
        if ((inst->GetCompletedEncounterMask() & (1u << BIT_TRIBUNAL_OF_AGES)) ||
            inst->GetBossState(BOSS_TRIBUNAL_OF_AGES) == DONE)
            return ObjectiveArriveResult::Done;

        bool const headsUp = HosHeadsUp(bot);

        // --- 2. running — hold the intercept line --------------------------
        //
        // RUNNING, NEVER Done, and the distinction is not cosmetic. This hook is
        // the Custom step of an ANCHORED event, and DungeonEventExecutor::RunStep
        // maps ObjectiveArriveResult::Done straight to StepResult::Done — the step
        // COMPLETES and the event advances to the next one. The next one here is
        // the 10206 gossip that skips the lore, so a Done returned while the
        // Tribunal is still running would abandon the garrison mid-defend and sit
        // on a gossip Brann will not offer for another four minutes.
        //
        // That is the opposite of the contract in HosDriveWave below, where Done
        // means "nothing to steer this tick, take the tick back" — because THAT
        // hook drives a Repeatable conditional event whose completion latches
        // nothing and re-fires on the next tick the predicate holds. The same enum
        // value means two different things on the two sides of that line, which is
        // exactly why HosHold() is used in one and never in the other.
        //
        // Starving the rotation is not a risk here: anchored events drive on the
        // NON-combat engine, so this hook only ever gets the ticks the wave driver
        // does not want. It is the Violet Hold's VhDriveDefend shape verbatim.
        if (inst->GetBossState(BOSS_TRIBUNAL_OF_AGES) == IN_PROGRESS || headsUp)
        {
            HosHoldPoint(bot);
            if (bot->GetExactDist2d(HOLD_X, HOLD_Y) <= HOS_DELIVERED_HOLD)
                HosArrive(bot);
            return ObjectiveArriveResult::Running;
        }

        // --- 3. recovery: Brann died, the escort has to run again ----------
        //
        // Reached only when the Tribunal is NOT_STARTED and no head is up. The two
        // conditions together are what distinguish "he died" from "we have not
        // started yet" — and the latter cannot occur here, because this objective
        // is ordered AFTER the escort objective whose completion marker is a head
        // being alive.
        Creature* brann = HosFindBrann(bot, /*radius*/ 250.0f);
        if (!brann)
        {
            // DespawnOrUnsummon(5s, 10s) — he is briefly absent after every death.
            // Hold at the camp and wait him out rather than walking anywhere.
            // Running, not Done, for the reason spelled out in rule 2.
            HosHoldPoint(bot);
            return ObjectiveArriveResult::Running;
        }

        {
            PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
            if (ai && !DcRun::Of(ai).Throttled(DcThrottle::HosTribunalLog, 15000))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Halls of Stone — the Tribunal is back at NOT_STARTED "
                         "with no heads up; Brann died. Re-running the escort from {} "
                         "({:.0f}yd away).",
                         bot->GetName(), bot->GetExactDist2d(brann));
            }
        }

        // Walk into interact range and re-take his gossip. Try the select FIRST and
        // close the gap only if it did not take: the reach test here and the core's
        // are not the same test — GetNPCIfCanInteractWith is bounding-radius aware,
        // so it accepts ranges a strict centre-to-centre gate rejects, and a hook
        // that refuses to try until its own stricter gate passes can sit one tick
        // of drift outside a range the server would have honoured. Trying costs one
        // packet pair and SelectGossip reports honestly (false while the menu is
        // not populated), so a miss simply falls through to the walk-in.
        if (bot->IsWithinDistInMap(brann, INTERACTION_DISTANCE))
        {
            HosArrive(bot);
            if (brann->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
                DungeonEventExecutor::SelectGossip(bot, brann, /*option*/ 0);
            return ObjectiveArriveResult::Running;
        }

        HosTravelToCreature(bot, brann, HOS_ENGAGE_CLOSE_LEASH);
        return ObjectiveArriveResult::Running;
    }

    // --- hook 23: HosDriveWave — the whole defend, one step ----------------
    //
    // ONE Custom step, not a step list, for the reason set out on the event row:
    // this encounter needs a standing PREFERENCE re-evaluated every tick, not a
    // sequence. Re-decided per tick, highest first:
    //
    //   0. SIDE EFFECT, never gates, never redirects: force-pull any live add
    //      within 30yd that has lost its combat. Narrow on purpose — every add
    //      arrives already in combat with the party, so this repairs strays rather
    //      than starting fights.
    //
    //   1. THE ADD CLOSEST TO BRANN, leashed to 40yd of him. The rule the whole
    //      encounter turns on: the adds are Taunt-wired to a passive NPC with
    //      regeneration off whose death is the only fail condition, so the tank's
    //      job is to be hit INSTEAD OF HIM, not to fight whatever is nearest.
    //
    //   2. (folded into 1 by the leash) never advance past the spawn points and
    //      never follow an add that is running away — an add outside the leash is
    //      not a threat to Brann and is not worth the party's position.
    //
    //   3. FINISH WHAT IS ALREADY ON US before moving. Everything within the band
    //      is fighting the party right now; walking away does not disengage it, it
    //      just puts the party's back to a mob that will re-acquire Brann.
    //
    //   4. Nothing in reach and nothing near Brann -> hold the intercept line.
    //
    // DELIBERATELY NOT HERE, and each for a stated reason:
    //
    //   * ANY ATTEMPT TO TOUCH THE THREE HEADS. NOT_SELECTABLE, faction 114,
    //     DISABLE_MOVE, NullCreatureAI. A driver that tried would burn ticks
    //     forever on a unit that cannot be damaged.
    //   * HAZARD DODGING. Searing Gaze and Dark Matter are DcHazardEmitter rows and
    //     the party's own vacate machinery (DungeonClearHazardVacate{Trigger,Action})
    //     walks bots out of them in BOTH engines. A second dodger here would fight
    //     that action for the one action per tick and neither would finish.
    //   * A KILL TARGET WHOSE DEATH IS PROGRESS. There isn't one: the clock is
    //     fixed at 300 seconds and no death shortens it.
    ObjectiveArriveResult HosDriveWave(Player* bot, AiObjectContext* context,
                                       DungeonBossInfo const& /*info*/)
    {
        if (!bot || !bot->GetMap())
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Done;

        // Not our turn: the Tribunal is over (or was never started). Stand down and
        // let hook 22 own the ending — it sits BELOW this event's rung, so it
        // cannot run while this event is due, and the predicate goes false within a
        // couple of seconds because EndTribunalFight despawns the heads and the
        // summons together.
        if ((inst->GetCompletedEncounterMask() & (1u << BIT_TRIBUNAL_OF_AGES)) ||
            inst->GetBossState(BOSS_TRIBUNAL_OF_AGES) == DONE)
            return ObjectiveArriveResult::Done;

        // --- 0. the standing force-pull -----------------------------------
        uint32 pulled = 0;
        {
            std::list<Creature*> adds;
            bot->GetCreatureListWithEntryInGrid(adds, HallsOfStoneWaveEntries(),
                                                HOS_PULL_RADIUS);
            for (Creature* c : adds)
                if (HosForcePull(bot, c))
                    ++pulled;
        }

        Creature* brann = HosFindBrann(bot, ARENA_SCAN);
        Creature* onBrann = HosAddNearestBrann(bot, brann);
        Creature* here = HosNearestAdd(bot, HOS_ENGAGE_BAND);

        // One line per bot per 3s carrying every number this hook decides on. A run
        // that goes wrong is diagnosed from this: "brannHp falling with onBrann
        // null" means the leash is too tight and adds are reaching him from outside
        // it; "onBrann set forever at a constant distance" means the party cannot
        // reach the intercept point; a rising add count with pulled 0 means the
        // force-pull guard is wrong.
        {
            PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
            if (ai && !DcRun::Of(ai).Throttled(DcThrottle::HosWaveLog, 3000))
            {
                LOG_DEBUG("playerbots.dungeonclear",
                          "DungeonClear: Halls of Stone — {} Tribunal: pulled {}, "
                          "brann {} hp {}%, onBrann {} {:.0f}yd-from-brann, here {} {:.0f}yd, "
                          "hold {:.0f}yd",
                          bot->GetName(), pulled,
                          brann ? "up" : "MISSING",
                          brann ? static_cast<uint32>(brann->GetHealthPct()) : 0u,
                          onBrann ? onBrann->GetName() : "none",
                          onBrann && brann ? onBrann->GetExactDist2d(brann) : -1.0f,
                          here ? here->GetName() : "none",
                          here ? bot->GetExactDist2d(here) : -1.0f,
                          bot->GetExactDist2d(HOLD_X, HOLD_Y));
            }
        }

        // --- 1. the add closest to Brann ----------------------------------
        if (onBrann)
        {
            if (bot->GetExactDist2d(onBrann) > HOS_DELIVERED_TARGET)
            {
                HosTravelToCreature(bot, onBrann, HOS_ENGAGE_CLOSE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            // Delivered — stop the glide first (see HosArrive), then commit. Both
            // calls are idempotent, so re-running them on ticks where the add has
            // drifted out of melee is safe.
            HosArrive(bot);
            HosForcePull(bot, onBrann);
            HosEngageTarget(bot, context, onBrann);

            // Out of combat nothing else closes the gap — an add that took aggro
            // from the healer is not walking to the tank, and there is no MoveChase
            // on the non-combat engine.
            if (!bot->IsInCombat() && bot->GetExactDist2d(onBrann) > HOS_ENGAGE_CLOSE)
            {
                HosTravelToCreature(bot, onBrann, HOS_ENGAGE_CLOSE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            return HosHold(bot);
        }

        // --- 3. finish what is already on us ------------------------------
        if (here)
        {
            // Retarget unless we are ALREADY on a live add inside the band. The
            // `if (!bot->GetVictim())` shape reads "has a victim" as "is fighting
            // the right thing", and a tank holding a stale victim — one that died,
            // or one it chased out of the band — then never commits to the mob in
            // its face and never enters combat at all.
            Unit* const victim = bot->GetVictim();
            if (!victim || !HosIsWaveAdd(victim) ||
                bot->GetExactDist2d(victim) > HOS_ENGAGE_BAND)
                HosEngageTarget(bot, context, here);

            if (!bot->IsInCombat() && bot->GetExactDist2d(here) > HOS_ENGAGE_CLOSE)
            {
                HosTravelToCreature(bot, here, HOS_ENGAGE_CLOSE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            HosArrive(bot);
            return HosHold(bot);
        }

        // --- 4. hold the intercept line -----------------------------------
        //
        // The quiet stretches: the first 52 seconds, and the gaps between waves.
        // Wait ON the line rather than wherever the last fight ended — that is the
        // entire positional value this driver adds, and the reason it must own the
        // tick in combat to do it.
        if (bot->GetExactDist2d(HOLD_X, HOLD_Y) > HOS_DELIVERED_HOLD)
        {
            HosHoldPoint(bot);
            return ObjectiveArriveResult::Running;
        }
        HosArrive(bot);
        return ObjectiveArriveResult::Done;
    }
}

// Ids 22 and 23. 22 garrisons the hold point for the Tribunal's fixed 300s and
// owns the Brann-died recovery; 23 is the wave driver. Referenced from
// HallsOfStoneEvents.cpp as DcHallsOfStone::HOOK_*.
void RegisterHallsOfStoneHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcHallsOfStone;
    ObjectiveHookRegistry::AddHook(out, HOOK_TRIBUNAL, &DriveHallsOfStoneTribunal);
    ObjectiveHookRegistry::AddHook(out, HOOK_WAVE, &HosDriveWave);
}
