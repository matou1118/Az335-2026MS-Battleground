/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <cmath>
#include <list>
#include <string>
#include <vector>

#include "CombatManager.h"
#include "Creature.h"
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

// The Violet Hold driver — the imperative half of map 608's clear.
//
// Five hooks, and only the last of them is a controller in the Black Morass
// sense. 15 starts the siege, 16/17/18 garrison the door camp and watch one
// boss-state slot each, and 19 (VhDriveWave) re-decides from live world state
// every tick for the whole 18-wave encounter. Like BmDriveWave it owns three
// layers the event framework normally supplies:
//
//   * MOVEMENT   — VhTravelTo / VhGarrison / the repath epsilon and reissue
//                  throttle. The events set StepsOwnMovement (which strips the
//                  driving action's per-tick ResolveEscortConflict, because that
//                  hold cancels a long-range spline the tick after it is issued),
//                  so the driver owns the whole glide lifecycle including
//                  cancelling it on delivery.
//   * ENGAGEMENT — VhEngageTarget / VhForcePull, so a mob parked at the door
//                  channelling on a trigger still gets handed to the combat
//                  engine, and so the tank commits to the KEEPER rather than to
//                  whichever add just spawned next to it.
//   * SELECTION  — VhSealDrainers / VhSelectKeeper / VhLiveBoss, over three
//                  disjoint entry classes with different kill semantics.
//
// The DECLARATIVE half of map 608 (event rows, the wave predicate, the roster
// patch, and the three shared entry lists this file consumes) stays in
// Data/Events/VioletHoldEvents.cpp, per the one-file-per-dungeon rule in
// DungeonEventTables.h. The numbers both halves must agree on live in
// namespace DcVioletHold in that header.
//
// WHAT MAKES THIS DUNGEON DIFFERENT FROM THE BLACK MORASS, and where the rules
// below come from:
//
//   * IT HAS A REAL CHOKEPOINT, AND CAMPING IT IS A TRAP. All six trash waypoint
//     paths converge on two points a yard apart at the door, so a party parked on
//     the landing does intercept every add for free. That is not how the Violet
//     Hold is played and it is not how it is won. A keeper portal pumps 3-4 adds
//     every 20 seconds for as long as its keeper lives, and the keeper stands at
//     the rim and never comes; a party at the door is therefore fighting the
//     pump's OUTPUT on the pump's schedule, indefinitely, while the seal takes a
//     point every three seconds from whatever slips the net. The way the dungeon
//     is actually run — by players and now by the driver — is to STATION ON THE
//     LIVE PORTAL: the adds are summoned within 2yd of it, so they are met at the
//     spawn instead of at the door, and the keeper is already in melee range the
//     moment it appears. The door camp survives as the EMERGENCY position (rule
//     2), for the runs where something did slip past.
//   * THE LOSS CONDITION IS READABLE, but not through GetData. _gateHealth has
//     no GetData case (user-avoid-core-changes rules out adding one), so the
//     driver reads the DRAIN instead of the LEVEL: every add at the door holds a
//     58040 aura on the Prison Door Seal, so the seal's own aura list names
//     exactly which creatures are draining it, right now. That is strictly
//     better than the Black Morass's count-bodies-in-a-ring proxy, and it is why
//     rule 2 can be precise instead of geometric.
//   * THE PUMP NEVER STOPS. A keeper portal summons 3 (+1 from wave 12) trash
//     every 20 seconds for as long as its keeper lives, and the keeper stands at
//     the portal and never aggros. There is no waiting this one out.

namespace
{
    using namespace DcVioletHold;

    // --- travel tuning -----------------------------------------------------

    // Beyond this a bare MovePoint is not trusted to deliver. The engine
    // PathGenerator caps a generated path at 74 polys / 74 points, so a
    // cross-arena request truncates — and it does so SILENTLY: the bot simply
    // stands still, with no failure to observe at the call site. Every portal in
    // this dungeon is 52-86yd from the camp, so every keeper hop is a long haul.
    constexpr float VH_LONG_HAUL = 30.0f;

    // "Is the glide in flight still aimed at the right place?" — deliberately NOT
    // the arrival leash, which is far tighter. A live keeper is a moving target
    // and a boss walking to its start position moves several yards a second, so
    // comparing those against an arrival leash would cancel and re-path the glide
    // every tick. What this DOES have to tell apart are the six rim portals,
    // which are 32-84yd from each other, so a coarse epsilon separates "same
    // destination, jittered" from "a different portal".
    //
    // It is a CEILING, not a constant, because this dungeon has hops shorter than
    // the epsilon itself. The Sinclari walk-in is 7yd; a stale escort destination
    // left over from the arrival glide sits well inside 12yd of it, so a flat
    // epsilon reads "gliding there already" and the driver issues nothing, for
    // ever. VhRepathEpsilon scales it to half the trip (floor 2yd) so the test can
    // never be wider than the movement it is gating.
    constexpr float VH_REPATH_EPSILON = 12.0f;

    float VhRepathEpsilon(float dist)
    {
        return std::min(VH_REPATH_EPSILON, std::max(2.0f, dist * 0.5f));
    }

    // Floor between two spline issues for the SAME destination. Out of combat the
    // escort-generator check in VhTravelTo is enough on its own; IN COMBAT it is
    // not, because the stock combat engine layers MoveChase back over the escort
    // slot whenever it wins a tick, and without a time floor the driver would
    // rebuild and re-issue a cross-arena route every single tick.
    constexpr uint32 VH_REISSUE_MS = 1500;

    // Camp leash — how far off the camp point the tank may drift before the
    // garrison walks it back. Loose enough that a tank in melee with something
    // that walked into the funnel is not dragged off it, tight enough that the
    // party stays between the adds and the seal.
    constexpr float VH_CAMP_LEASH = 8.0f;

    // Tight camp leash for a travel destination we want to be STANDING ON (a
    // portal we are waiting to spawn its keeper), where there is no fight yet and
    // the loose band buys nothing.
    constexpr float VH_ARRIVE_LEASH = 6.0f;

    // --- DELIVERY BANDS: where the driver stops steering ------------------
    // The driver's job is CROSS-ARENA delivery, not the last few yards. Inside
    // these bands it stands down and hands the tick back, and the stock combat
    // engine's MoveChase closes the remaining distance while the tank fights.
    //
    // Two separate failures on map 269 made this necessary, both the same mistake
    // in different clothing — the driver claiming ticks it did not need.
    // Engine::DoNextAction runs exactly ONE action per tick and this driver sits
    // ABOVE the stock combat movers in the combat engine, so a hook that reports
    // Running every tick leaves the tank standing in melee with no rotation and
    // no threat while the DPS pull aggro and the party wipes. And a tight arrival
    // leash re-triggers travel over a couple of yards, so a tank at melee range
    // would break off, walk two yards, and re-enter combat, forever.
    constexpr float VH_DELIVERED_TARGET = 25.0f;
    constexpr float VH_DELIVERED_CAMP   = 20.0f;

    // Band for the two positions the driver PARKS on rather than fights at: the
    // live portal (rule 6) and the staging point (rule 7). Much tighter than the
    // two above because there is no fight in progress at either — the whole value
    // of standing on a portal is being inside the 2yd bubble its adds are summoned
    // into, and a 20yd "delivered" would leave the party watching the spawn from
    // across the room. It cannot cause the walk-two-yards-and-re-engage thrash the
    // wide bands exist to prevent, because rule 4 (below) outranks both of these
    // rungs the instant anything hostile is within reach.
    constexpr float VH_DELIVERED_PARK = 10.0f;

    // How close the driver walks the tank when it is OUT OF COMBAT and has already
    // decided to fight something.
    //
    // The delivery bands above hand the last stretch to the stock combat engine's
    // MoveChase, and that is right while the tank is fighting. It is wrong when it
    // is not: an unaggroed tank is on the NON-combat engine, where there is no
    // MoveChase to hand anything to, and this driver's own hold claims the tick.
    // "Delivered" at 25yd would then leave it standing at range swinging at
    // nothing — the standing-still twin of the walking-home bug the hold exists to
    // fix. So out of combat the driver closes to melee itself.
    //
    // The arrival band is deliberately WIDER than the re-issue leash it travels
    // with: equal numbers would leave a ring in which the travel call declines to
    // re-issue ("already there") while the arrival test still reads "too far", and
    // the tank would hold the tick standing in it.
    constexpr float VH_ENGAGE_CLOSE       = 4.0f;
    constexpr float VH_ENGAGE_CLOSE_LEASH = 2.0f;

    // --- the wave rules ----------------------------------------------------

    // Force-pull radius, measured from the BOT. Deliberately NOT arena-wide: the
    // pull's whole job is to hand the combat engine a mob it would otherwise
    // ignore — one standing next to the party casting on a trigger instead of at
    // anybody. Pulling one from across the room instead buys nothing (it walks at
    // the tank, loses it, and violet_hold_trashAI::EnterEvadeMode sends it back to
    // a home 2.5yd from the door to resume channelling) and costs a party dragged
    // into a fight it did not choose. 30yd is "everything we are standing among":
    // from the camp it covers the whole convergence funnel including both
    // endpoints, and from a camped portal it covers every fresh spawn (summoned
    // within 2yd) while falling well short of the door 52-86yd away.
    constexpr float VH_PULL_RADIUS = 30.0f;

    // Rule 4's band — "something is already on top of us, finish it before moving".
    // Deliberately the SAME number as the force-pull radius: everything the driver
    // drags into combat at rung 0 is something it then has to stand and fight, and
    // two different numbers would give the party a ring it pulls from and refuses
    // to fight in. This is the rung that stops the portal camp from becoming a
    // conga line — without it, a party that has just killed a keeper walks off to
    // the next rim portal 3s later with that portal's last trash batch still
    // swinging at its back, and those adds break off and walk to the seal.
    constexpr float VH_ENGAGE_BAND = VH_PULL_RADIUS;

    // How many live 58040 casters on the Prison Door Seal count as "the seal is
    // dirty enough to abandon the station and go home for".
    //
    // The aura ticks once per 3s and the gate has 100 points, so one add alone
    // takes 300s to lose the run, two take 150s, three take 100s. A keeper wave
    // delivers three at a time.
    //
    // TWO, and the reasoning changed shape when the party stopped waiting at the
    // door. It used to be a question of when to leave a position that was already
    // intercepting everything; it is now the leak detector under a party that is
    // deliberately somewhere else. ONE is still wrong: a single straggler costs
    // the gate a point every three seconds, and turning a party at the rim around
    // for that is a 100-170yd round trip that hands the portal a free pumping
    // cycle — the yo-yo is the same failure the station exists to end, in a new
    // costume. THREE would mean a whole batch is already parked on the door.
    // This is the first number to re-derive from live drain data (VhDriveWave's
    // per-3s log line carries the count on every tick).
    constexpr uint32 VH_SEAL_DIRTY_MIN = 2;

    // The count at which the seal outranks EVERYTHING, including a keeper the
    // party is standing on.
    //
    // VH_KEEPER_COMMIT is a distance, so it is a commitment with no clock on it:
    // a party parked next to a keeper it cannot kill — one it cannot reach through
    // geometry, one whose fight has gone wrong, a tank kiting in circles — holds
    // rule 2 off indefinitely, and under the station that "next to the keeper" is
    // now the NORMAL state of a keeper wave rather than the exceptional one. Four
    // drainers is 75 seconds from losing the run outright, which is well past the
    // point where finishing the keeper first is the better trade.
    constexpr uint32 VH_SEAL_CRITICAL = 4;

    // Fallback ring around the convergence midpoint, used ONLY when the Prison
    // Door Seal creature cannot be found (it is a NOT_SELECTABLE world spawn at
    // (1823.72, 803.86, 48.93), so this should never happen inside the hold). The
    // aura read is the primary and it is exact; this is a count of bodies.
    constexpr float VH_SEAL_RING = 12.0f;

    // Once the tank is this close to its locked keeper, FINISH IT — the seal
    // diversion stands down. Abandoning a keeper we have already crossed the room
    // for is strictly worse than letting the gate tick a few more times: the
    // keeper is the pump, and it dies in seconds once the party is on it. Sized
    // against the geometry rather than copied from map 269 — the nearest portal
    // is 51.8yd from the camp, so a 50yd commit (the Black Morass value) would
    // latch the instant the tank left the camp and make rule 2 unreachable for
    // half the portals. 30yd is past the midpoint of the shortest crossing.
    constexpr float VH_KEEPER_COMMIT = 30.0f;

    // Sinclari's gossip is an ordinary NPC interaction. Player::
    // GetNPCIfCanInteractWith gates on IsWithinDistInMap(INTERACTION_DISTANCE),
    // which is 5.5yd BOUNDING-RADIUS AWARE — so centre-to-centre it is 5.5 plus
    // both combat reaches, and 4.5 is comfortably inside with a tick of drift to
    // spare.
    //
    // THE TRAVEL AIM IS SINCLARI HERSELF, and this is the fix for the two runs
    // (tr-20260826-224756-1 / -224909-2) that stalled at "Enter the Violet Hold"
    // having never moved a yard. The first cut copied the Black Morass walk-in and
    // aimed at a GetNearPoint on the bot's side of her, with a 2yd arrival leash on
    // that point. GetNearPoint returns a spot `her size + approach + bot size` out
    // — about 6yd for two humanoids at approach 3 — so for a tank parked at the
    // objective anchor 7.3yd away the aim point was ~1.3yd from its feet, INSIDE
    // the leash. VhTravelTo read that as "arrived", issued nothing, and the hook
    // logged `dist 7.3yd, reach 4yd` identically for five minutes.
    //
    // That whole near-point dance is Black Morass machinery for a 20yd PROXIMITY
    // trigger, where the thing being gated is a 3D distance to a creature standing
    // on different ground. Here the gate is an ordinary interact range, so aim at
    // the NPC and let MovePoint stop where it stops. The leash below exists only
    // so a bot standing on top of her does not re-path; it is deliberately far
    // under VH_GOSSIP_REACH so it can never again swallow the approach.
    constexpr float VH_GOSSIP_REACH = 4.5f;
    constexpr float VH_GOSSIP_LEASH = 1.5f;

    // PER-BOT throttled travel log, not a single global static: with several
    // instances running concurrently a global would make every consecutive line
    // come from a DIFFERENT bot, which makes the one measurement that matters
    // ("is this bot's distance falling tick over tick?") impossible to read.
    // The slot lives on the bot's own run state — see Util/DcThrottle.h for why
    // not in a file-scope thread_local map keyed by GUID.
    void VhTravelLog(Player* bot, char const* what, float dist, std::string const& detail)
    {
        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || DcRun::Of(botAI).Throttled(DcThrottle::VhTravelLog, 3000))
            return;
        LOG_DEBUG("playerbots.dungeonclear",
                 "DungeonClear: Violet Hold — {} travel: {} (dist {:.1f}yd){}{}",
                 bot->GetName(), what, dist, detail.empty() ? "" : " — ", detail);
    }

    // Travel to an arena point, long-haul aware and safe to call every tick while
    // IN COMBAT. The Black Morass BmTravelTo verbatim in structure, because the
    // constraint is identical: every destination worth travelling to here is
    // 52-86yd away across an open bowl with no intervening room to chunk against,
    // and a bare MovePoint at that range fails SILENTLY.
    //
    // NOTE what is deliberately NOT here: an `if (bot->isMoving()) return;` guard.
    // In combat the bot is essentially always moving under MoveChase, so such a
    // guard makes the driver a no-op for the entire encounter. This hook owns the
    // tick (DcRel::EventDueCombat, above the stock combat movers) precisely so it
    // CAN take the bot off its current target and walk it to the keeper;
    // VH_REISSUE_MS is what keeps that from becoming spline spam.
    void VhTravelTo(Player* bot, float x, float y, float z, float leash)
    {
        if (!bot)
            return;

        float const dist = bot->GetExactDist(x, y, z);
        if (dist <= leash)
            return;  // arrived

        // An escort glide already in flight owns the bot. Let it finish if it is
        // headed here; drop it if it is stale — a route to the PREVIOUS portal
        // (closed since we launched) would otherwise ride all the way out before
        // the bot could react to the portal that is actually open.
        float const epsilon = VhRepathEpsilon(dist);

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

        // Same-destination re-issue floor (see VH_REISSUE_MS): the check above
        // cannot see a glide the combat engine has since layered MoveChase over.
        if (PlayerbotAI* const issuer = GET_PLAYERBOT_AI(bot))
            if (DcRun::Of(issuer).ThrottledIssue(DcThrottle::VhTravelIssue, x, y, z,
                                                 epsilon, VH_REISSUE_MS))
                return;

        if (dist > VH_LONG_HAUL)
        {
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                ChunkedPathfinder::Result const path = LongRangePathfinder::Build(bot, x, y, z);
                // Outcome of the long haul, per bot, throttled. This is the ONLY
                // way to tell "the glide was issued and is running" from "the
                // route could not be built" from "the spline was issued and
                // something cancelled it".
                VhTravelLog(bot,
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
                        // hold is one continuous bowl with a ramp, so this should
                        // never fire; deliver what we have.
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
                    VhTravelLog(bot, issued ? "spline ISSUED" : "spline REFUSED (paused/<2pts)",
                                dist, std::to_string(points.size()) + " pts");
                    if (issued)
                        return;
                }
            }
            // No navmesh route: fall through. MovePoint will not deliver at this
            // range either, but the force-pull below remains the backstop once
            // mobs are actually next to us.
        }

        bot->GetMotionMaster()->MovePoint(0, x, y, z,
                                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);
    }

    // Travel to a CREATURE, snapping the aim point onto the navmesh first.
    //
    // Unlike the Black Morass, several destinations in this room float above the
    // floor the party has to stand on: column probes against the live 608 mmtile
    // put portal 0's scripted position 4.0yd above the only walkable surface
    // under it and portal 4's 2.9yd above its own, because the portal model hangs
    // in the air. Feeding those raw z values to LongRangePathfinder asks it for a
    // route to a point with no polygon under it. NavmeshSnap's vertical extent is
    // a FIXED 10yd (dc-boss-anchor-snap-vertical-extent), which covers every gap
    // in this room with margin; if the snap misses, the raw position is still the
    // best guess available and is used unchanged.
    void VhTravelToCreature(Player* bot, Creature* c, float leash)
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
        VhTravelTo(bot, x, y, z, leash);
    }

    // Back to the door landing. The EMERGENCY position (rule 2) and the hold
    // while a post-cleanup Sinclari is respawning, not the between-waves station —
    // that is VhStage.
    void VhGarrison(Player* bot)
    {
        VhTravelTo(bot, CAMP_X, CAMP_Y, CAMP_Z, VH_CAMP_LEASH);
    }

    // The between-waves station: the middle of the arena floor. Used by rule 7 and
    // by the three defend objectives' garrison, which are the same act seen from
    // either side of the wave predicate — the 3s gap between one portal dying and
    // the next opening flips that predicate false, and if the two rungs disagreed
    // about where "wait" is, every wave transition would tug the party a few yards
    // toward the other one's answer.
    void VhStage(Player* bot)
    {
        VhTravelTo(bot, STAGE_X, STAGE_Y, STAGE_Z, VH_CAMP_LEASH);
    }

    // ARRIVED: kill the delivery glide.
    //
    // The events set StepsOwnMovement, which strips the driving action's per-tick
    // ResolveEscortConflict — that hold was what cancelled the driver's own
    // long-range spline the tick after it was issued. Taking that responsibility
    // means taking the other half too: NOTHING else stops the glide, so deciding
    // "delivered" and simply yielding leaves an 80yd escort spline in flight and
    // it carries the tank on to wherever it was originally aimed. (Live on map
    // 269: the tank ran past the keeper, onto the portal, then turned around and
    // walked back to engage.) ResolveEscortConflict only acts while an ESCORT
    // glide is the active generator, so this is a no-op once stopped and never
    // perturbs the MoveChase that closes the last few yards.
    void VhArrive(Player* bot)
    {
        DcMovement::ResolveEscortConflict(bot);
    }

    // Delivered at a destination: keep the tick or hand it over?
    //
    // IN COMBAT, hand it over. That is the whole reason the yield exists — the
    // stock combat engine needs the tick to pick a target, swing, cast and hold
    // threat, and it only gets one action per tick.
    //
    // OUT OF COMBAT, keep it. The rung below is the defend objective's garrison
    // (DcRel::AtObjective), whose job between waves is to hold the camp; yield to
    // it while standing on a portal waiting for a keeper to spawn — the quiet 10s
    // after a portal opens — and it cheerfully walks the tank straight back off
    // the portal. Holding the tick here is what pins the party on the portal
    // through that gap, which is the entire point of camping it. Nothing is lost:
    // out of combat there is no rotation to run.
    ObjectiveArriveResult VhHold(Player* bot)
    {
        return bot->IsInCombat() ? ObjectiveArriveResult::Done
                                 : ObjectiveArriveResult::Running;
    }

    // Force a Violet Hold mob into combat with the bot.
    //
    // Less universally necessary than its Black Morass counterpart and kept for
    // one specific shape. The wave adds DO aggro on the way in — they are
    // npc_escortAI walking a waypoint list with an ordinary ScriptedAI
    // MoveInLineOfSight — but the moment one reaches the door,
    // CreatureStartAttackDoor() drops its escort state, sets IMMUNE_TO_NPC and
    // starts an AOE cast on a NOT_SELECTABLE trigger. From then on it has no
    // victim and no threat table, points at nothing, and simply stands on the
    // landing draining the gate. That is the state the party can walk right past.
    //
    // EngageWithTarget adds forced threat (or SetInCombatWith for a threat-less
    // unit), which hands the mob to the ordinary combat engine; AttackStart makes
    // its AI actually swing rather than merely hold threat. Deliberately NOT
    // fixed by loosening AttackersValue::IsPossibleTarget or the
    // `!bot->IsHostileTo()` gate in EngageDirect: both are shared by every
    // dungeon's engage path and their strictness is load-bearing elsewhere.
    bool VhForcePull(Player* bot, Creature* c)
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
    // VhForcePull is the other half of a pull and is not a substitute for this
    // one: EngageWithTarget/AttackStart make the CREATURE attack the BOT, and
    // nothing in that direction ever gives the bot a victim. A player does not
    // auto-retaliate — something has to call Player::Attack. Normally that is
    // stock combat's own targeting, but the driver has to be sure the tank commits
    // to the KEEPER (or the boss) specifically, and to commit on the tick it
    // arrives rather than whenever stock targeting next re-picks.
    //
    // Mirrors the in-range tail of DungeonClearEngageActionBase::EngageDirect
    // minus the parts only an Action can do. Idempotent by the GetVictim() guard,
    // so calling it every tick never fights stock combat's target management once
    // the tank is already on the right mob — and it is exactly what retargets the
    // tank off Ichoron the moment he goes NOT_SELECTABLE and onto a globule.
    bool VhEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
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
        // Player::Attack alone just gives the bot a victim and the CORE then
        // drives auto-attack swings with no AI involvement, so a bot that is
        // force-attacked without the flip sits on the NON-combat engine, where no
        // class rotation exists to run, and melees the mob down one white hit at a
        // time with no threat abilities. Deliberately NOT inside the !alreadyOnIt
        // branch: once a bot has been left attacking on the wrong engine
        // GetVictim() already equals the target, so gating the flip on the target
        // CHANGE would latch the broken state permanently. ChangeEngine no-ops
        // when the engine already matches.
        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
        return !alreadyOnIt;
    }

    // EXACTLY who is draining the door seal, right now.
    //
    // _gateHealth has no GetData case, so the drain LEVEL is unreadable without a
    // core change. The drain ITSELF is not: violet_hold_trashAI::
    // CreatureStartAttackDoor casts 58040 on the Prison Door Seal (30896) and
    // ClearDoorSealAura removes it `(spell, me->GetGUID())`, i.e. one aura
    // instance per casting creature, keyed by caster. So walking the seal's
    // applied-aura list for 58040 and resolving each caster gives the precise set
    // of live drainers — better than any geometric proxy, and it names the mobs
    // the party has to kill rather than merely counting bodies in a volume.
    //
    // Returns the drainers in `out`. Falls back to a body count in a ring around
    // the convergence midpoint ONLY if the seal creature itself cannot be found;
    // an EMPTY aura list on a seal we CAN see is a true reading ("nothing is
    // draining") and must not be second-guessed.
    uint32 VhSealDrainers(Player* bot, std::vector<Creature*>& out)
    {
        out.clear();
        Creature* seal = bot->FindNearestCreature(NPC_PRISON_DOOR_SEAL, ARENA_SCAN,
                                                  /*alive*/ true);
        if (!seal)
        {
            std::list<Creature*> nearby;
            bot->GetCreatureListWithEntryInGrid(nearby, VioletHoldWaveEntries(), ARENA_SCAN);
            uint32 count = 0;
            for (Creature* c : nearby)
                if (c && c->IsAlive() &&
                    c->GetExactDist2d(SEAL_X, SEAL_Y) <= VH_SEAL_RING)
                {
                    out.push_back(c);
                    ++count;
                }
            return count;
        }

        Unit::AuraApplicationMap const& applied = seal->GetAppliedAuras();
        auto const range = applied.equal_range(SPELL_DESTROY_DOOR_SEAL);
        for (auto it = range.first; it != range.second; ++it)
        {
            AuraApplication const* app = it->second;
            Aura* aura = app ? app->GetBase() : nullptr;
            if (!aura)
                continue;
            Unit* caster = ObjectAccessor::GetUnit(*bot, aura->GetCasterGUID());
            Creature* c = caster ? caster->ToCreature() : nullptr;
            if (c && c->IsAlive())
                out.push_back(c);
        }
        return static_cast<uint32>(out.size());
    }

    // The portal keeper this bot is working, LOCKED until it dies.
    //
    // Selection has to be stable. A per-tick "nearest keeper" re-decides as the
    // tank moves, and with a second portal opening 3s after the first closes that
    // flip strands the party between two of them — the shape the Black Morass
    // reported as "they fall apart any time two portals are open". Only ONE
    // portal exists at a time here, but the lock still matters across the
    // portal-death boundary: the moment a keeper dies its portal despawns and the
    // next opens on the far rim, and without the lock a tank two yards from a
    // corpse would immediately be re-aimed 84yd away.
    //
    // The latch lives on the bot's own run state. It used to be a file-scope
    // thread_local map keyed by GUID, and a LOCK is the worst thing to put there:
    // when a map moves between MapUpdater pool threads the table reads empty, the
    // lock silently lapses, and the very re-selection this exists to prevent
    // happens anyway. See Util/DcThrottle.h.
    Creature* VhSelectKeeper(Player* bot)
    {
        PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
        ObjectGuid* locked = botAI ? &DcRun::Of(botAI).vhKeeperLock : nullptr;

        if (locked && !locked->IsEmpty())
        {
            if (Creature* held = ObjectAccessor::GetCreature(*bot, *locked))
                if (held->IsAlive())
                    return held;
            locked->Clear();
        }

        // ARENA-WIDE rather than banded around a portal, deliberately. Only ONE
        // Teleportation Portal exists at a time (the instance schedules the next
        // EVENT_SUMMON_PORTAL only after the current one is defeated), so there is
        // never a second keeper to confuse this with — and a keeper is summoned
        // within 2yd of its portal and NEVER MOVES (the portal channels 58012 on
        // it in place and its SmartAI carries no waypoints), so a band would only
        // add a way to lose sight of it.
        std::list<Creature*> keepers;
        bot->GetCreatureListWithEntryInGrid(keepers, VioletHoldKeeperEntries(), ARENA_SCAN);

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* k : keepers)
        {
            if (!k || !k->IsAlive())
                continue;
            float const d = bot->GetExactDist2d(k);
            if (!best || d < bestDist)
            {
                best = k;
                bestDist = d;
            }
        }
        if (best && locked)
            *locked = best->GetGUID();
        return best;
    }

    // The released prisoner (or Cyanigosa) the party owes a fight, or nullptr.
    //
    // NONE OF THESE EVER COME TO YOU: every boss AI on this map overrides
    // MoveInLineOfSight to {} (checked, all nine structs in boss_*.cpp), and the
    // instance MovePoints each one to a fixed position 45-88yd from the camp. So
    // unlike the trash, a released boss is only ever fought because the driver
    // walked the party to it.
    //
    // Cyanigosa is included by the same IsReleased test rather than by a special
    // case: she spawns NON_ATTACKABLE at (1930.28, 804.41, 52.41) on a rim ledge,
    // MoveJumps to the middle of the room, and the instance clears the flag 12.5s
    // later. Testing the flag therefore does two jobs at once — it waits out the
    // transform, and it means the driver NEVER paths to her z 52.4 spawn ledge,
    // because by the time she reads released she is already on the floor.
    Creature* VhLiveBoss(Player* bot)
    {
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, VioletHoldPrisonerEntries(), ARENA_SCAN);
        if (Creature* cy = bot->FindNearestCreature(NPC_CYANIGOSA, ARENA_SCAN, /*alive*/ true))
            found.push_back(cy);

        // STICK TO WHAT WE ARE ALREADY FIGHTING. Nearest-first is the right way to
        // CHOOSE, and the wrong way to KEEP: Erekem is released with two guards
        // that fight at (1858.85, 855.07) and (1891.93, 863.39) with him between
        // them, so once the party is in among all three "nearest" flips from tick
        // to tick and the driver re-points the tank at a different one every time
        // (mgt-rotunda-target-thrash). Preferring the live victim, while it is
        // still a released prisoner, makes the choice sticky for free — and it
        // releases automatically in the one case that matters, Ichoron going
        // NOT_SELECTABLE, because IsReleased then reads false for him.
        if (Unit* victim = bot->GetVictim())
            if (Creature* vc = victim->ToCreature())
                if (IsReleased(vc))
                    for (Creature* c : found)
                        if (c == vc)
                            return vc;

        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* c : found)
        {
            if (!IsReleased(c))
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

    // The portal the party should be standing on, or nullptr.
    //
    // Only ONE Teleportation Portal (31011) exists at a time — the instance
    // schedules the next EVENT_SUMMON_PORTAL only once the current one is defeated
    // — so this is a lookup, not a choice, and needs none of VhSelectKeeper's
    // locking.
    //
    // THE VISIBILITY TEST IS THE POINT, and it is what tells the two wave kinds
    // apart without guessing. npc_vh_teleportation_portal rolls RAND(KEEPER_OR_
    // GUARDIAN, ELITES) 10s after it opens and does not announce which:
    //
    //   * KEEPER: summons its guardian/keeper at +10s and then 3 (+1 from wave 12)
    //     trash EVERY 20s, forever, staying visible the whole time. The party
    //     wants to be standing here for all of it — the keeper spawns in melee
    //     range and every trash batch is summoned within 2yd of the party's feet.
    //   * ELITES: summons its 2-3 elites at +10s and IMMEDIATELY calls
    //     SetVisible(false), then lingers, alive but invisible, until they die.
    //     Its whole output is already in the room and walking to the door. There
    //     is nothing left here to camp, and camping it would park the party on an
    //     empty rim spot while the elites drain the seal behind them.
    //
    // So an invisible portal reads as "spent" and hands the tick down to rules 4
    // and 6, which fight the elites where they actually are. Grid searchers ignore
    // server-side visibility (AllCreaturesOfEntryInRange tests entry and range
    // only), so the flag has to be tested here explicitly — it does not filter
    // itself out of the scan.
    Creature* VhLivePortal(Player* bot)
    {
        std::list<Creature*> portals;
        bot->GetCreatureListWithEntryInGrid(portals, NPC_TELEPORTATION_PORTAL, ARENA_SCAN);
        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* p : portals)
        {
            if (!p || !p->IsAlive() || !p->IsVisible())
                continue;
            float const d = bot->GetExactDist2d(p);
            if (!best || d < bestDist)
            {
                best = p;
                bestDist = d;
            }
        }
        return best;
    }

    // Is this unit one of the encounter's live wave mobs?
    //
    // The membership half of VhNearestWaveHostile, split out because the assist
    // rung and the retarget guard both need to ask it of a unit they are already
    // holding rather than of everything inside a radius.
    bool VhIsWaveHostile(Unit* u)
    {
        Creature* const c = u ? u->ToCreature() : nullptr;
        if (!c || !c->IsAlive() || c->GetEntry() == NPC_CYANIGOSA)
            return false;
        for (uint32 const entry : VioletHoldWaveEntries())
            if (c->GetEntry() == entry)
                return true;
        return false;
    }

    // The nearest wave hostile that SOMEBODY ELSE in the party is fighting.
    //
    // Every other probe in this file is measured from the TANK, and that is the
    // hole the assist rung closes. A portal summons its adds within 2yd of itself
    // and they pick their own first target, so the ordinary case is a mob that
    // walks straight past the tank and onto the healer — and until the tank is
    // itself in combat there is no rung that names that fight. It is not enough
    // that rule 4 covers the tank's own 30yd band either: the fight drifts, the
    // band empties, and the driver goes home while four bots are still swinging.
    //
    // The PvE COMBAT REFS are the authoritative "who has this member in combat"
    // list — the same set DcCombatFlag::ScanCombatHolders and the teardown
    // snapshot walk — and they are what a threatless door-channeller shows up in.
    // The attacker set and the member's own victim are folded in as supersets for
    // anything mid-swing that has not registered a reference yet. Deduping is
    // unnecessary: the pick is a nearest-of, so seeing the same mob twice costs a
    // comparison and changes nothing.
    //
    // Bounded to ARENA_SCAN and to the bot's own map, because a group member can
    // be standing in another instance copy entirely.
    Creature* VhPartyAssistTarget(Player* bot)
    {
        Group* const group = bot->GetGroup();
        if (!group)
            return nullptr;

        Unit* const victim = bot->GetVictim();
        Creature* best = nullptr;
        float bestDist = 0.0f;
        bool sticky = false;
        auto const consider = [bot, victim, &best, &bestDist, &sticky](Unit* u)
        {
            if (sticky || !VhIsWaveHostile(u))
                return;
            Creature* const c = u->ToCreature();
            if (!c->IsInWorld() || c->GetMap() != bot->GetMap())
                return;
            float const d = bot->GetExactDist2d(c);
            if (d > ARENA_SCAN)
                return;
            // STICK TO WHAT WE ARE ALREADY FIGHTING, for the same reason
            // VhLiveBoss does: this rung re-runs every tick and the tank is
            // MOVING, so a plain nearest-of would re-point it at a different mob
            // of the batch every few yards and it would build threat on none of
            // them. Any candidate is a fine answer; the one already being hit is
            // the stable one.
            if (c == victim)
            {
                best = c;
                bestDist = d;
                sticky = true;
                return;
            }
            if (!best || d < bestDist)
            {
                best = c;
                bestDist = d;
            }
        };

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive())
                continue;
            if (member->GetMap() != bot->GetMap())
                continue;
            for (auto const& kv : member->GetCombatManager().GetPvECombatRefs())
                if (CombatReference* const cref = kv.second)
                    consider(cref->GetOther(member));
            for (Unit* const attacker : member->getAttackers())
                consider(attacker);
            consider(member->GetVictim());
        }
        return best;
    }

    // The nearest live wave hostile within `radius` of the bot, or nullptr.
    // Used by rule 4 and rule 7 to hand the tank a victim, so a door-channelling
    // add the stock targeting will not pick still gets fought.
    Creature* VhNearestWaveHostile(Player* bot, float radius)
    {
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, VioletHoldWaveEntries(), radius);
        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            // Cyanigosa is in the wave list so the activation predicate sees her
            // the moment she lands; she is a BOSS and rule 1 owns her.
            if (c->GetEntry() == NPC_CYANIGOSA)
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

    // --- the start / restart gossip ---------------------------------------
    // Shared by hook 15 (the start objective) and hooks 16/17/18 (the wipe and
    // gate-failure recovery path), because they are the same act: the instance is
    // sitting at NOT_STARTED and only Sinclari's gossip moves it.
    //
    // IDEMPOTENT BY CONSTRUCTION — it re-reads instance state every tick and never
    // latches, so a restarted Drive cannot double-fire. It also cannot pick the
    // WRONG option: menu 9997's two options are mutually exclusive by condition
    // (option 0 "start" only while NOT_STARTED, option 1 "late-join teleport" only
    // while IN_PROGRESS), and every caller has already tested NOT_STARTED, so
    // index 0 is the start option. Never forges DoAction(ACTION_START_INSTANCE)
    // directly: the instance takes it guarded on NOT_STARTED, but going through
    // the gossip is what keeps the whole flow blizzlike and self-healing.
    ObjectiveArriveResult VhDriveStart(Player* bot)
    {
        Creature* sinclari = bot->FindNearestCreature(NPC_SINCLARI, ARENA_SCAN, /*alive*/ true);
        if (!sinclari)
        {
            // InstanceCleanup DespawnOrUnsummon(0ms, 3s)s her, so she is briefly
            // absent after every reset. Hold and wait her out.
            VhGarrison(bot);
            return ObjectiveArriveResult::Running;
        }

        float const dist = bot->GetExactDist(sinclari);

        // TRY THE GOSSIP FIRST, then close the gap if it did not take. The two are
        // in this order (rather than "walk, then talk") because the reach test
        // here and the core's are not the same test: GetNPCIfCanInteractWith is
        // bounding-radius aware, so it accepts ranges this one rejects, and a
        // hook that refuses to try until its own stricter gate passes can sit one
        // tick of drift outside a range the server would have honoured. Trying
        // costs one packet pair and SelectGossip reports honestly — it returns
        // false while the menu has not populated — so the failure simply falls
        // through to the walk-in below and the next tick tries again from closer.
        bool selected = false;
        if (dist <= VH_GOSSIP_REACH)
        {
            VhArrive(bot);

            // She loses UNIT_NPC_FLAG_GOSSIP the instant DoAction fires and does
            // not get it back until EVENT_START_ENCOUNTER 15s later (by which
            // point the instance is IN_PROGRESS and every caller has already
            // returned Done). So a missing flag here means the select landed and
            // we are between ticks — hold rather than hammering the menu.
            if (!sinclari->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            {
                // Log it: this is the one branch that can hold for ever without
                // moving, and silence here is what made the first two stalls
                // unreadable. Her template carries npcflag 1 and her spawn row
                // overrides nothing, so a persistent miss means the select
                // already landed (or something else cleared the flag) — either
                // way the run wants to know which.
                PlayerbotAI* const flagAI = GET_PLAYERBOT_AI(bot);
                if (flagAI && !DcRun::Of(flagAI).Throttled(DcThrottle::VhSinclariFlagLog, 10000))
                {
                    LOG_DEBUG("playerbots.dungeonclear",
                             "DungeonClear: Violet Hold — {} is in reach of Sinclari "
                             "({:.1f}yd) but she has no gossip flag; holding",
                             bot->GetName(), dist);
                }
                return ObjectiveArriveResult::Running;
            }

            selected = DungeonEventExecutor::SelectGossip(bot, sinclari, /*option*/ 0);
            if (selected)
                return ObjectiveArriveResult::Running;
        }

        // Per-bot throttled, not a single function-local static: two bots drove
        // this objective in the same run (tr-20260826-224756-1) and a shared
        // throttle interleaves their lines, which makes the one measurement that
        // matters — is THIS bot's distance falling tick over tick? — unreadable.
        {
            PlayerbotAI* const walkAI = GET_PLAYERBOT_AI(bot);
            if (walkAI && !DcRun::Of(walkAI).Throttled(DcThrottle::VhSinclariWalkLog, 10000))
            {
                LOG_DEBUG("playerbots.dungeonclear",
                         "DungeonClear: Violet Hold — walking {} to Lieutenant Sinclari to "
                         "start the siege (dist {:.1f}yd, reach {:.1f}yd, gossip {})",
                         bot->GetName(), dist, VH_GOSSIP_REACH,
                         dist <= VH_GOSSIP_REACH ? "tried, menu not ready" : "out of reach");
            }
        }

        VhTravelTo(bot, sinclari->GetPositionX(), sinclari->GetPositionY(),
                   sinclari->GetPositionZ(), VH_GOSSIP_LEASH);
        return ObjectiveArriveResult::Running;
    }

    // --- hook 15: DriveVioletHoldStart -------------------------------------
    // The Custom step of the persistent "Enter the Violet Hold" objective. One
    // job: get the instance out of NOT_STARTED. Done the moment it is, which is
    // also what makes it impossible for this hook to reach the menu in which
    // index 0 is the late-join teleport.
    ObjectiveArriveResult DriveVioletHoldStart(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        if (inst->GetData(DATA_ENCOUNTER_STATUS) != NOT_STARTED)
            return ObjectiveArriveResult::Done;

        return VhDriveStart(bot);
    }

    // --- hooks 16 / 17 / 18: the three defend objectives -------------------
    // Between waves the tank garrisons the STAGING POINT in the middle of the
    // room; the wave event's rung (31, or DcRel::EventDueCombat 61 under fire)
    // preempts this garrison (30) exactly while anything is up, so this hook only
    // ever runs in the quiet. The quiet is short and frequent — a portal's death
    // opens the next one 3 SECONDS later — which is why this and rule 7 of the
    // wave driver must name the SAME waiting position: two different answers would
    // pull the party a few yards back and forth at every wave transition, on every
    // one of the eighteen waves.
    //
    // COMPLETION IS THE BOSS-STATE SLOT, never the completed-encounter mask. The
    // released pair is rolled per instance (a random pick over the BOSS_MORAGG
    // .. BOSS_ZURAMAT range) and
    // instance_encounters only names Erekem and Moragg, so a run that draws
    // Zuramat and Xevozz sets NO DungeonEncounter bit for either — the instance's
    // own DATA_1ST_BOSS / DATA_2ND_BOSS / DATA_CYANIGOSA slots are the only
    // truthful record. (boss-state-index-spaces,
    // ac-encounter-credit-is-per-credit-entry.)
    //
    // RESTART, not failure, on NOT_STARTED. Unlike the Black Morass — where
    // Medivh's shield hitting 0 makes the run unwinnable for 300s and the driver
    // ends it outright — a Violet Hold failure is fully recoverable:
    // InstanceCleanup despawns the field, reopens the main door, respawns
    // Sinclari WITH her gossip flag back, and rolls _waveCount to the last
    // completed checkpoint (0 / 6 / 12). So the right response to NOT_STARTED
    // mid-run is to walk back to her and start it again, from the checkpoint. It
    // is logged loudly with the wave count, because a run that does this
    // repeatedly is a run that is losing the seal and the log line is the only
    // evidence of it (_gateHealth is not exposed).
    ObjectiveArriveResult VhDriveDefend(Player* bot, uint32 bossStateSlot, char const* label)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        if (inst->GetBossState(bossStateSlot) == DONE)
            return ObjectiveArriveResult::Done;

        uint32 const status = inst->GetData(DATA_ENCOUNTER_STATUS);
        if (status == DONE)
            return ObjectiveArriveResult::Done;  // Cyanigosa already dead this lockout

        if (status == NOT_STARTED)
        {
            PlayerbotAI* const restartAI = GET_PLAYERBOT_AI(bot);
            if (restartAI && !DcRun::Of(restartAI).Throttled(DcThrottle::VhRestartLog, 15000))
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Violet Hold — instance is back at NOT_STARTED during "
                         "'{}' (wave {}); the seal failed or the party wiped. Re-starting from "
                         "the checkpoint via Sinclari.",
                         label, inst->GetData(DATA_WAVE_COUNT));
            }
            return VhDriveStart(bot);
        }

        VhStage(bot);
        return ObjectiveArriveResult::Running;
    }

    ObjectiveArriveResult DriveVioletHoldFirstPrisoner(Player* bot, AiObjectContext* /*context*/,
                                                       DungeonBossInfo const& /*info*/)
    {
        return VhDriveDefend(bot, BOSS_STATE_1ST, "First Prisoner");
    }

    ObjectiveArriveResult DriveVioletHoldSecondPrisoner(Player* bot, AiObjectContext* /*context*/,
                                                        DungeonBossInfo const& /*info*/)
    {
        return VhDriveDefend(bot, BOSS_STATE_2ND, "Second Prisoner");
    }

    ObjectiveArriveResult DriveVioletHoldCyanigosa(Player* bot, AiObjectContext* /*context*/,
                                                   DungeonBossInfo const& /*info*/)
    {
        return VhDriveDefend(bot, BOSS_STATE_CYANIGOSA, "Cyanigosa");
    }

    // --- hook 19: VhDriveWave — the whole encounter, one step --------------
    //
    // ONE Custom step, not a step list. A list can only say "do these in order
    // and block on each"; this encounter needs a standing preference re-evaluated
    // every tick as portals open, prisoners release and adds arrive. Re-decided
    // per tick, highest first:
    //
    //   0. SIDE EFFECT, never gates, never redirects: force-pull every wave
    //      hostile within 30yd that is not already fighting. A mob that has
    //      reached the door has dropped its escort state and is casting on a
    //      NOT_SELECTABLE trigger, so it points at nobody and the party can stand
    //      next to it doing nothing while the gate drains.
    //
    //   1. A RELEASED PRISONER (or Cyanigosa) IS UP -> travel to it and engage.
    //      None of them ever aggro (MoveInLineOfSight is {} on all nine boss AIs)
    //      and all of them sit 45-88yd from the door, so the fight only happens
    //      because the driver walks the party there. It outranks everything below
    //      because waves 6, 12 and 18 spawn NO PORTAL — the pump is off, the only
    //      adds left are leftovers, and the wave cannot advance until the boss
    //      dies. Ichoron's shattered-bubble window falls out for free: IsReleased
    //      tests NOT_SELECTABLE, so he stops being "the boss" for those 15s and
    //      rule 4 hands the tank an Ichor Globule instead of leaving it in
    //      drop-target limbo on an untargetable unit.
    //
    //   2. THE SEAL IS DIRTY (>= VH_SEAL_DIRTY_MIN live 58040 casters on it) and
    //      we are not already committed to a keeper -> go home to the door camp
    //      and clean it. This is the ONLY rung that goes back to the door, and it
    //      is the safety net under the portal station: everything the party misses
    //      while it is out at the rim ends up here, visible by name, and this rung
    //      is what collects it. Bounded by VH_KEEPER_COMMIT so it cannot ping-pong
    //      the party off a keeper it already crossed the room for — a keeper dies
    //      in seconds once the party is on it, and stopping the pump is worth more
    //      than the handful of gate points the crossing costs — and that bound is
    //      itself bounded by VH_SEAL_CRITICAL, because a distance commit has no
    //      clock on it and a keeper fight that goes wrong would otherwise hold the
    //      rung off until the gate hit zero.
    //
    //   3. A LIVE PORTAL KEEPER -> travel to it and engage. It is the ONLY
    //      off-switch for the 20-second add pump, and it never moves. The choice
    //      is LOCKED until it dies, so the next portal opening 3s later on the far
    //      rim cannot flip the target mid-approach.
    //
    //   4. ANYTHING HOSTILE WITHIN VH_ENGAGE_BAND -> stand where we are and fight
    //      it. This rung is what makes stationing on a portal safe. Without it the
    //      party leaves for the next rim spot the instant the current portal dies,
    //      with that portal's last trash batch still swinging at its back — and
    //      those adds, no longer engaged, walk to the seal. It also covers the
    //      elite waves end to end: the portal goes invisible the moment it summons
    //      them, so rule 6 stands down and this rung fights them wherever they are
    //      between the rim and the door.
    //
    //   5. A GROUPMATE IS FIGHTING A WAVE HOSTILE -> travel to that mob and take
    //      it. Every other probe here is measured from the tank, and the ordinary
    //      case is a wave that aggroes somebody else: the mob is summoned at the
    //      portal, picks the healer, and the tank — with an empty 30yd band and no
    //      combat flag — used to read the room as quiet and go home.
    //
    //   6. A LIVE, UNSPENT PORTAL -> travel to it and STAND ON IT. The station.
    //      Adds are summoned within 2yd of the portal and the keeper spawns inside
    //      melee range, so a party that is already there meets the entire wave at
    //      the spawn point instead of chasing its output to the door. This is the
    //      rung that replaced "wait at the chokepoint": see the file header for
    //      why the tidy-looking chokepoint reading loses the run.
    //
    //   7. Otherwise -> hold the staging point in the middle of the room, and give
    //      the tank a victim if something is standing next to it that stock
    //      targeting has not picked.
    //
    // WHY RULE 2 SITS ABOVE THE STATION AND RULE 4 BELOW THE KEEPER. The seal is
    // the loss condition and nothing else in the room is, so it outranks every
    // preference about where to fight. The keeper outranks "fight what is here"
    // because the adds in the party's face ARE the keeper's output: killing the
    // keeper is the only thing that makes them stop coming, and a party that
    // finishes the current batch first simply meets the next one 20s later.
    //
    // RETURN CONTRACT — the tick is the scarce resource. Engine::DoNextAction
    // executes exactly ONE action per tick and this driver sits above the stock
    // combat movers in the COMBAT engine. Running means "I am steering the tank
    // right now" and claims the tick, which is what lets it take the tank off
    // whatever it is fighting and walk it across the room. Done means "nothing to
    // steer" and YIELDS. Getting that backwards wipes parties. Never Blocked:
    // the event is Optional + Repeatable so a timeout simply re-fires it fresh.
    //
    // AND "DONE" ONLY YIELDS TO THE COMBAT ENGINE WHEN THE BOT IS IN COMBAT.
    // That was this driver's most expensive assumption. Out of combat the tick
    // goes down the NON-combat ladder instead, where the rung below EventDue (31)
    // is the defend objective's garrison (AtObjective 30) — VhDriveDefend, whose
    // whole body is VhStage. So an unaggroed tank standing next to a fight that
    // yielded its tick was not handing it to a rotation; it was handing it to a
    // 40-50yd spline back to the staging point. Every rung that stands its ground
    // therefore ends in VhHold (Done in combat, Running out of it), never in a
    // bare Done. Only rule 7 may yield unconditionally, and only because the
    // garrison it yields to agrees with it about where "wait" is.
    ObjectiveArriveResult VhDriveWave(Player* bot, AiObjectContext* context,
                                      DungeonBossInfo const& /*info*/)
    {
        if (!bot || !bot->GetMap())
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Done;

        // Not our turn: hooks 15-18 own the start, the restart and the ending.
        // Stand down and let them run — they sit BELOW this event's rung, so they
        // cannot run while it is due, and the predicate goes false within a couple
        // of seconds of a cleanup because InstanceCleanup despawns the whole field.
        if (inst->GetData(DATA_ENCOUNTER_STATUS) != IN_PROGRESS)
            return ObjectiveArriveResult::Done;

        // --- 0. the standing force-pull -----------------------------------
        uint32 pulled = 0;
        {
            std::list<Creature*> adds;
            bot->GetCreatureListWithEntryInGrid(adds, VioletHoldWaveEntries(), VH_PULL_RADIUS);
            for (Creature* c : adds)
                if (c && c->GetEntry() != NPC_CYANIGOSA && VhForcePull(bot, c))
                    ++pulled;
        }

        std::vector<Creature*> drainers;
        uint32 const draining = VhSealDrainers(bot, drainers);

        Creature* boss = VhLiveBoss(bot);
        Creature* keeper = VhSelectKeeper(bot);
        Creature* portal = VhLivePortal(bot);
        Creature* assist = VhPartyAssistTarget(bot);
        bool const atKeeper = keeper && bot->GetExactDist2d(keeper) <= VH_KEEPER_COMMIT;

        // One line per bot per 3s, carrying every number this hook decides on. A
        // run that goes wrong is diagnosed from this: "draining > 0 forever" means
        // the sweep is not landing; "portal set, keeper null forever" means the
        // station is being held on a portal whose keeper never arrives; the wave
        // counter standing still with a keeper alive means the party never
        // reached it.
        {
            PlayerbotAI* const waveAI = GET_PLAYERBOT_AI(bot);
            if (waveAI && !DcRun::Of(waveAI).Throttled(DcThrottle::VhWaveLog, 3000))
            {
                LOG_DEBUG("playerbots.dungeonclear",
                         "DungeonClear: Violet Hold — {} wave {}/18: draining {}, pulled {}, "
                         "portal {:.0f}yd, keeper {}, atKeeper {}, boss {}, assist {} {:.0f}yd",
                         bot->GetName(), inst->GetData(DATA_WAVE_COUNT), draining, pulled,
                         portal ? bot->GetExactDist2d(portal) : -1.0f,
                         keeper ? keeper->GetName() : "none", atKeeper,
                         boss ? boss->GetName() : "none",
                         assist ? assist->GetName() : "none",
                         assist ? bot->GetExactDist2d(assist) : -1.0f);
            }
        }

        // --- 1. a released prisoner / Cyanigosa is up ----------------------
        if (boss)
        {
            if (bot->GetExactDist2d(boss) > VH_DELIVERED_TARGET)
            {
                VhTravelToCreature(bot, boss, VH_ARRIVE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            // Delivered — stop the glide first (see VhArrive), then commit. Both
            // calls are idempotent, so re-running them on the ticks where the boss
            // has drifted out of melee is safe.
            VhArrive(bot);
            VhForcePull(bot, boss);
            VhEngageTarget(bot, context, boss);
            return VhHold(bot);
        }

        // --- 2. clean the door seal ---------------------------------------
        // The keeper commit (atKeeper) buys the party the right to finish a keeper
        // it has already crossed the room for; VH_SEAL_CRITICAL takes that right
        // back when the gate is close enough to failing that no keeper is worth it.
        if (draining >= VH_SEAL_DIRTY_MIN && (!atKeeper || draining >= VH_SEAL_CRITICAL))
        {
            if (bot->GetExactDist2d(CAMP_X, CAMP_Y) > VH_DELIVERED_CAMP)
            {
                // Same band the guard just tested, so the two agree on one number.
                VhTravelTo(bot, CAMP_X, CAMP_Y, CAMP_Z, VH_DELIVERED_CAMP);
                return ObjectiveArriveResult::Running;
            }
            // Inside the band. The drainers were force-pulled above and are running
            // at us; name one explicitly so the tank commits to a mob stock
            // targeting would happily ignore, then hand the tick to combat.
            Creature* target = nullptr;
            for (Creature* c : drainers)
                if (c && c->IsAlive())
                {
                    target = c;
                    VhEngageTarget(bot, context, c);
                    break;
                }
            // Out of combat, walk in — same reason as rule 4 below. A drainer that
            // the force-pull did not reach is standing on the landing channelling
            // at a trigger and will never come to the tank.
            if (target && !bot->IsInCombat() &&
                bot->GetExactDist2d(target) > VH_ENGAGE_CLOSE)
            {
                VhTravelToCreature(bot, target, VH_ENGAGE_CLOSE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            VhArrive(bot);
            // VhHold, not a bare Done: the camp is 37yd from the staging point, so
            // a tank that reaches the door without taking a hit yet would be walked
            // straight back off it by the garrison. See the return contract above.
            return VhHold(bot);
        }

        // --- 3. kill the portal keeper ------------------------------------
        if (keeper)
        {
            if (bot->GetExactDist2d(keeper) > VH_DELIVERED_TARGET)
            {
                VhTravelToCreature(bot, keeper, VH_ARRIVE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            VhArrive(bot);
            VhForcePull(bot, keeper);
            VhEngageTarget(bot, context, keeper);
            return VhHold(bot);
        }

        // --- 4. finish what is already on us before moving -----------------
        //
        // The rung that makes the station work. Everything within the band was
        // force-pulled at rung 0 and is fighting the party right now; walking away
        // from it does not disengage it, it just puts the party's back to a mob
        // that will break off, resume its waypoint list, and drain the seal. So
        // stand still, make sure the tank has a victim, and hold: in combat that
        // hands the tick to the rotation, and out of combat it keeps the garrison
        // from walking the tank off a fight it has not been hit by yet.
        if (Creature* here = VhNearestWaveHostile(bot, VH_ENGAGE_BAND))
        {
            // Retarget unless we are ALREADY on a live wave mob inside the band.
            // The old `if (!bot->GetVictim())` reads "has a victim" as "is
            // fighting the right thing", and a tank holding a stale victim — one
            // that died, or one it chased out of the band — then never commits to
            // the mob in its face and never enters combat at all, which is the
            // state the rung below used to walk it home in.
            Unit* const victim = bot->GetVictim();
            if (!victim || !VhIsWaveHostile(victim) ||
                bot->GetExactDist2d(victim) > VH_ENGAGE_BAND)
                VhEngageTarget(bot, context, here);

            // IN COMBAT this is a pure stand-and-fight: MoveChase owns the last
            // few yards and the rotation needs the tick, so nothing is issued and
            // the tick goes back. OUT OF COMBAT nothing else will close the gap —
            // a mob that took aggro from the healer is not walking to the tank and
            // there is no MoveChase on the non-combat engine — so the driver walks
            // the tank into melee itself. Without this half the hold below just
            // freezes it at range instead of sending it home, which is a different
            // bug wearing the same fix.
            if (!bot->IsInCombat() && bot->GetExactDist2d(here) > VH_ENGAGE_CLOSE)
            {
                VhTravelToCreature(bot, here, VH_ENGAGE_CLOSE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            VhArrive(bot);
            return VhHold(bot);
        }

        // --- 5. assist a groupmate who is already fighting -----------------
        //
        // THE TANK'S JOB, and the rung this driver was missing. A portal's adds
        // are summoned within 2yd of it and pick their own first target, so the
        // normal case is a wave that aggroes the healer or a DPS and never touches
        // the tank. Rules 0 and 4 both measure from the tank, so on those ticks
        // the driver saw an empty room, fell through to the staging rung, and
        // issued a 40-50yd spline home while the rest of the party fought the wave
        // it had just walked to (tp-20260828-205158-2: 142 travels to the staging
        // point across five runs, the tank 4th or 5th into combat in half its
        // engagements and absent from whole waves).
        //
        // Sits BELOW rule 4 because a mob already on the tank beats one that is
        // not, and ABOVE the portal station because the fight happening now beats
        // pre-positioning for the one after it. It cannot fight rule 2 for the
        // door-channellers: those hold nobody in combat, so they never appear here.
        if (assist)
        {
            if (bot->GetExactDist2d(assist) > VH_DELIVERED_TARGET)
            {
                VhTravelToCreature(bot, assist, VH_ARRIVE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            VhArrive(bot);
            VhForcePull(bot, assist);
            VhEngageTarget(bot, context, assist);
            return VhHold(bot);
        }

        // --- 6. station on the live portal --------------------------------
        //
        // THE DEFAULT POSITION, and the change that made this dungeon play the way
        // it is meant to. The portal's adds are summoned within 2yd of it and its
        // keeper spawns in melee range, so a party standing here meets the whole
        // wave at the spawn point — where it can be killed before it walks — while
        // a party at the door meets it 30-40s later, one batch at a time, forever.
        //
        // The 10s before the portal declares itself is not wasted: on a keeper wave
        // the party is already on top of the keeper when it appears (rule 3 then
        // takes over with a 3yd approach instead of an 80yd one), and on an elite
        // wave the portal goes invisible at the same instant, VhLivePortal stops
        // returning it, and rules 4, 5 and 7 fight the elites where they are.
        //
        // Held with a Running out of combat (see VhHold): the defend objectives'
        // garrison sits one rung below and would otherwise walk the party off the
        // portal during exactly the quiet window that standing on it is for.
        if (portal)
        {
            if (bot->GetExactDist2d(portal) > VH_DELIVERED_PARK)
            {
                VhTravelToCreature(bot, portal, VH_ARRIVE_LEASH);
                return ObjectiveArriveResult::Running;
            }
            VhArrive(bot);
            return VhHold(bot);
        }

        // --- 7. hold the staging point ------------------------------------
        //
        // No portal, no keeper, no boss and nothing in reach: the 3s gap between
        // portals, the 35s after a released prisoner dies, and the tail of a wave
        // whose last add is still walking in from the rim. Wait in the middle of
        // the room rather than at the door — it is 23.5yd closer to the average rim
        // portal and it is still astride every trash path — and keep naming a
        // victim, because an add that reached the seal points at nobody and stock
        // targeting will not pick it up on its own.
        if (bot->GetExactDist2d(STAGE_X, STAGE_Y) > VH_DELIVERED_PARK)
        {
            VhStage(bot);
            return ObjectiveArriveResult::Running;
        }
        VhArrive(bot);
        if (!bot->GetVictim())
            if (Creature* nearby = VhNearestWaveHostile(bot, VH_PULL_RADIUS))
                VhEngageTarget(bot, context, nearby);
        return ObjectiveArriveResult::Done;
    }
}

// Ids 15-19. 15 starts the siege, 16/17/18 are the three defend objectives (one
// per instance boss-state slot — a Custom step is handed a default-constructed
// DungeonBossInfo, so one shared hook could not tell them apart), 19 is the wave
// driver. Referenced from VioletHoldEvents.cpp as DcVioletHold::HOOK_*.
void RegisterVioletHoldHooks(ObjectiveHookRegistry::HookTable& out)
{
    using namespace DcVioletHold;
    ObjectiveHookRegistry::AddHook(out, HOOK_START, &DriveVioletHoldStart);
    ObjectiveHookRegistry::AddHook(out, HOOK_DEFEND_1ST, &DriveVioletHoldFirstPrisoner);
    ObjectiveHookRegistry::AddHook(out, HOOK_DEFEND_2ND, &DriveVioletHoldSecondPrisoner);
    ObjectiveHookRegistry::AddHook(out, HOOK_DEFEND_CYANIGOSA, &DriveVioletHoldCyanigosa);
    ObjectiveHookRegistry::AddHook(out, HOOK_DRIVE_WAVE, &VhDriveWave);
}
