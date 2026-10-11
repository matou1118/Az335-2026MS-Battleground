/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// --- Halls of Lightning (map 602) — the Slag Furnace transit ---------------
//
// The imperative half of the leg between General Bjarngrim and Volkhan: the one
// stretch of this dungeon the ordinary clear cannot cross, because it has no
// driver in there at all.
//
// WHY THERE IS NO DRIVER. DcCombatFlag::MayDrive is false while anything in the
// party is engaged and Advance is registered ONLY in the non-combat engine.
// Fourteen Slag (28585) line the pit walkway on a TWENTY-SECOND respawn with an
// 8yd wander, and every one of them is inside aggro range of the route line — so
// party engagement never drops for the no-engage grace window, and the leg is not
// slow, it is stopped. Blackwing Lair's Suppression Rooms, one tier down.
//
// AND KILLING THEM IS NEGATIVE PROGRESS. `On Just Died -> Blast Wave` (23113 /
// heroic 22424) is 10yd and -50% movement speed for 6s, so a party that clears
// its way across halves its own crossing speed for the whole crossing.
//
// SO THE LEG IS A TRANSIT, NOT A CLEAR: one body, brakes off, crossed under
// fire. What is NOT skipped is the two Unbound Firestorms at the foot of the
// climb out and the Blistering Steamragers on the ledge above — 3600s respawns,
// so a kill there is progress that stays bought — and the driver stands and
// fights those on purpose.
//
// THE ARITHMETIC IS NOT HERE. It is the SHARED, map-free kernel in
// Util/DcSuppressionTransitDecision.h, used unchanged: this file is glue (grid
// scans, the party walk, the splines, the telemetry) exactly as
// BlackwingLairDriver.cpp is. Three of the kernel's four holds are live here —
// Gathering, PackTrailing and Elite. The fourth (Disarm) is Blackwing Lair's
// Suppression Devices; there is nothing on map 602 to disarm, so it is passed a
// radius of 0 and a distance of -1, which is the kernel's own "no such probe".
//
// THE RETURN CONTRACT IS BLACKWING LAIR'S, and getting it backwards wipes
// parties:
//
//   Running => "I am steering." Claims the tick.
//   Done    => "Nothing to steer." YIELDS the tick (the stepsOwnMovement branch
//              in DcRunEventAction) so the stock combat engine can fight: pick a
//              target, swing, cast, hold threat.
//
// EVERY HOLD YIELDS. That is not an optimisation — a hold is precisely a tick on
// which the party should be fighting rather than walking.

namespace
{
    using namespace DcHallsOfLightning;

    // The driver's slice of the authored row: anchors 5..18, so cursor 0 IS the
    // staging point at the top of the descent and the LAST anchor IS the mid
    // ledge. A MIDDLE slice, unlike Blackwing Lair's tail one — the party still
    // has a hairpin and a second ramp to walk after this crossing, and those are
    // ordinary ground that must not be steered by a driver whose route ends
    // behind them.
    DcTransit::RouteView const* TransitRoute()
    {
        return DcTransit::Route(MAP_ID, NPC_VOLKHAN, TRANSIT_STAGE_ANCHOR_INDEX,
                                TRANSIT_END_ANCHOR_INDEX);
    }

    // How far the FURTHEST living member is from the CURSOR, and how many are
    // inside the gather radius OF THE CURSOR. One walk of the group for both.
    //
    // BOTH AGAINST THE CURSOR, and that is Blackwing Lair's measured fix rather
    // than a tidy-up: a gather quorum measured against the LEADER asks a question
    // the pack forms a different answer to, because during the gather the cursor
    // is pinned to the staging point while the leader keeps walking, and the two
    // distances ADD.
    //
    // Living members ON THIS MAP only: a corpse-running member is the rez
    // ladder's business, and a member who never zoned in must not pin the leg.
    struct HolPackView
    {
        float  trailDist = -1.0f;  // <0 = the leader is alone
        uint32 living = 0;         // living members on this map, leader included
        uint32 nearCursor = 0;     // ...of which, inside the gather radius of the cursor
        uint32 followers = 0;      // living members on this map, leader EXCLUDED
        uint32 outsideLeash = 0;   // ...of which, further than the pack leash from it
    };

    HolPackView HolPack(Player* bot, float cursorX, float cursorY, float cursorZ,
                        float gatherRadius, float packLeash)
    {
        HolPackView v;
        if (!bot)
            return v;

        v.living = 1;
        v.nearCursor = bot->GetExactDist(cursorX, cursorY, cursorZ) <= gatherRadius ? 1 : 0;

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
            ++v.followers;

            float const d = member->GetExactDist(cursorX, cursorY, cursorZ);
            if (d <= gatherRadius)
                ++v.nearCursor;
            if (d > v.trailDist)
                v.trailDist = d;
            if (packLeash > 0.0f && d > packLeash)
                ++v.outsideLeash;
        }
        return v;
    }

    // Nearest LIVE thing worth stopping for, or -1.
    //
    // Unbound Firestorms and Blistering Steamragers ONLY. The Slags are not a
    // reason to stand still: they are back twenty seconds after they die, and
    // every death snares the party that killed them, so a hold on them is a hold
    // that pays for itself in the wrong direction. The tank face-pulls what walks
    // into it and the party fights on the move — that is what NO_STOP on the
    // route and OwnsThePull on the event row exist to allow.
    float HolNearestElite(Player* bot)
    {
        if (!bot)
            return -1.0f;

        static std::vector<uint32> const kElites = { NPC_UNBOUND_FIRESTORM,
                                                     NPC_BLISTERING_STEAMRAGER };
        std::list<Creature*> found;
        bot->GetCreatureListWithEntryInGrid(found, kElites, TRANSIT_SCAN);

        float best = -1.0f;
        for (Creature* c : found)
        {
            if (!c || !c->IsAlive())
                continue;
            float const d = bot->GetExactDist(c);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    // The per-tick telemetry line, throttled. Without it a failed run says nothing
    // about WHICH mechanism is still biting — "the pack never formed" reads
    // exactly like "the driver never got a tick" in a log that only shows a party
    // standing in a pit.
    void HolTransitLog(Player* bot, DcRunState& st, DcSuppressionTransit::Inputs const& in,
                       DcSuppressionTransit::Verdict const& v, HolPackView const& pack,
                       uint32 anchorCount)
    {
        if (st.Throttled(DcThrottle::TransitLog, TRANSIT_TELEMETRY_MS))
            return;

        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] HoL slag furnace — cursor {}/{} ({:.1f}yd), pack {}/{} near the "
                  "cursor, {} of {} outside, trail {:.1f}yd (leash {:.0f}), elite {:.1f}yd "
                  "-> {}{}",
                  bot->GetName(), in.cursorIndex, anchorCount ? anchorCount - 1 : 0,
                  in.distToCursor, pack.nearCursor, pack.living, in.packOutside, in.packLiving,
                  in.trailDist, in.packLeash, in.nearestEliteDist,
                  v.complete ? "AT THE MID LEDGE"
                             : (v.advance ? "advancing" : DcSuppressionTransit::HoldName(v.hold)),
                  v.timedOut ? " (WATCHDOG)" : "");
    }
}

ObjectiveArriveResult DriveSlagFurnaceTransit(Player* bot, AiObjectContext* context,
                                              DungeonBossInfo const& /*info*/)
{
    if (!bot || !context || bot->GetMapId() != MAP_ID)
        return ObjectiveArriveResult::Done;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return ObjectiveArriveResult::Done;

    DcRunState& st = DcRun::Of(context);

    DcTransit::RouteView const* const rt = TransitRoute();
    if (!rt)
    {
        // No authored route: there is nothing to run a cursor down, and inventing
        // one across this geometry is how a party ends up on a z -13.9 sheet
        // ([[ac-map601-flat-gridheight-zero]] — map 602 is in that family). Yield
        // and let the ordinary ladder have the leg back.
        st.ClearTransit();
        return ObjectiveArriveResult::Done;
    }

    uint32 const now = getMSTime();
    std::vector<WaypointHint> const& hints = rt->hints;
    std::vector<DcSuppressionTransit::Anchor> const& route = rt->anchors;

    uint32 const anchorCount = static_cast<uint32>(route.size());
    uint32 const lastAnchor = anchorCount - 1;

    // --- arm the crossing --------------------------------------------------
    if (!st.transitArmedMs)
    {
        st.transitArmedMs = now;
        st.transitCursorIndex = 0;
        st.transitHoldSinceMs = 0;
        st.transitHoldReason = 0;
        st.transitHoldTimedOut = false;

        // YOU CANNOT GATHER AT A POINT YOU HAVE ALREADY PASSED, and DISTANCE IS
        // NOT THE TEST FOR THAT — position ALONG THE ROUTE is. A straight-line
        // test cannot tell "30yd short of the staging point" from "30yd past it",
        // and on this leg the second case is a party already on the descent: the
        // cursor would be pinned to an anchor BEHIND them and the pack hold would
        // measure everyone against it forever. That is the shape that cost
        // Blackwing Lair three of five raids their whole crossing.
        //
        // So both are asked, and either one skips the gather.
        float const toStage = bot->GetExactDist(TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z);
        uint32 const armCursor = DcSuppressionTransit::ResolveCursor(
            route, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            /*stored*/ 1, TRANSIT_CURSOR_RESYNC_DIST);

        st.transitGathered = toStage > TRANSIT_STAGE_SKIP_DIST || armCursor > 1;
        if (st.transitGathered)
            st.transitCursorIndex = armCursor;

        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] HoL slag furnace — crossing the pit ({} anchors, {:.0f}yd to the "
                 "staging point, projected anchor {}){}",
                 bot->GetName(), anchorCount, toStage, armCursor,
                 st.transitGathered ? " — already past the staging point, skipping the gather"
                                    : "");
    }

    // --- where are we ------------------------------------------------------
    //
    // The cursor is the leader's projection onto the route, monotone-clamped
    // against what we stored (see ResolveCursor) — EXCEPT while the gather gate is
    // still shut, where it is pinned to the staging anchor so the party forms on
    // the point it is standing on rather than on a leg already down the ramp.
    uint32 const projected = DcSuppressionTransit::ResolveCursor(
        route, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        std::max<uint32>(st.transitCursorIndex, 1), TRANSIT_CURSOR_RESYNC_DIST);

    // ...and the pin is re-checked EVERY TICK, not only at the arm. A leader can
    // be shoved down the descent after arming — dragged by a fight, carried by a
    // spline already in flight — and the arm-time answer would keep the cursor
    // parked at the top behind it for the rest of the leg.
    if (!st.transitGathered && projected > 1)
    {
        st.transitGathered = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] HoL slag furnace — the party is already {} anchor(s) down the "
                 "descent; abandoning the gather and crossing from here",
                 bot->GetName(), projected);
    }

    uint32 cursor = st.transitGathered ? projected : 0u;
    if (cursor > lastAnchor)
        cursor = lastAnchor;

    WaypointHint const& target = hints[cursor];

    // --- the snapshot ------------------------------------------------------
    //
    // THE GATHER RADIUS HAS A FLOOR, and the floor is the ring a pack rung would
    // park the party on. Nothing walks members back on this map (the follower
    // half of Blackwing Lair's transit is deliberately not generalised here — see
    // HallsOfLightningEvents.cpp), so the floor is inert today; it is applied
    // anyway because the two numbers are a RELATIONSHIP, and the day a pack rung
    // is added is not the day to rediscover [[dc-party-gate-must-read-the-rungs-ring]].
    float const gatherRadius =
        std::max(TRANSIT_GATHER_RADIUS,
                 DcSuppressionTransit::GatherRadiusFloor(TRANSIT_PACK_LEASH, 0.0f, 0.0f));

    HolPackView const pack =
        HolPack(bot, target.x, target.y, target.z, gatherRadius, TRANSIT_PACK_LEASH);

    DcSuppressionTransit::Inputs in;
    in.cursorIndex = cursor;
    in.anchorCount = anchorCount;
    in.distToCursor = bot->GetExactDist(target.x, target.y, target.z);
    in.arriveRadius = target.arriveRadius;
    in.gathered = st.transitGathered;
    in.quorumMet = pack.living <= 1 ||
                   static_cast<float>(pack.nearCursor) >=
                       static_cast<float>(pack.living) * TRANSIT_GATHER_QUORUM;
    in.topped = true;  // telemetry only in the kernel; nothing here reads it
    in.trailDist = pack.trailDist;
    in.packLeash = TRANSIT_PACK_LEASH;
    in.packOutside = pack.outsideLeash;
    in.packLiving = pack.followers;
    in.packQuorum = TRANSIT_GATHER_QUORUM;
    in.nearestEliteDist = HolNearestElite(bot);
    in.eliteHoldRadius = TRANSIT_ELITE_HOLD_RADIUS;
    // No Suppression Devices on this map. -1 / 0 is the kernel's "no such probe".
    in.nearestArmedDeviceDist = -1.0f;
    in.disarmHoldRadius = 0.0f;
    in.nowMs = now;
    in.armedSinceMs = st.transitArmedMs;
    in.holdSinceMs = st.transitHoldSinceMs;
    in.holdReason = st.transitHoldReason;
    in.gatherTimeoutMs = TRANSIT_GATHER_TIMEOUT_MS;
    in.packHoldTimeoutMs = TRANSIT_PACK_HOLD_TIMEOUT_MS;
    in.eliteHoldTimeoutMs = TRANSIT_ELITE_HOLD_TIMEOUT_MS;
    in.disarmHoldTimeoutMs = 0;

    DcSuppressionTransit::Verdict const v = DcSuppressionTransit::Decide(in);

    HolTransitLog(bot, st, in, v, pack, anchorCount);

    // --- act ---------------------------------------------------------------
    if (v.openGatherGate)
    {
        st.transitGathered = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] HoL slag furnace — {} of {} party members formed up at the top of "
                 "the descent{} — crossing",
                 bot->GetName(), pack.nearCursor, pack.living,
                 v.timedOut ? " (GATHER TIMEOUT — going with what we have)" : "");
    }

    if (v.complete)
    {
        // The mid ledge. Kill the glide first: the event sets StepsOwnMovement, so
        // nothing else cancels it, and deciding "arrived" while an escort spline is
        // still in flight carries the tank straight on past the ledge and into the
        // hairpin (the Black Morass lesson, measured on map 269).
        DcMovement::ResolveEscortConflict(bot);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] HoL slag furnace — the party is out of the pit and on the mid "
                 "ledge ({} member(s) alive on the map)",
                 bot->GetName(), pack.living);
        st.ClearTransit();
        return ObjectiveArriveResult::Done;
    }

    // ONCE PER RELEASE, not once per tick: an Elite watchdog release LATCHES (the
    // verdict keeps reporting the hold with its original clock so the leg keeps
    // walking), so `v.timedOut` stays true for as long as the condition it gave up
    // on persists.
    bool const watchdogFired = v.timedOut && !v.openGatherGate;
    if (watchdogFired && !st.transitHoldTimedOut)
        LOG_WARN("playerbots.dungeonclear",
                 "[DC:{}] HoL slag furnace — watchdog released a '{}' hold at anchor {} "
                 "(trail {:.1f}yd, elite {:.1f}yd) -> walking on",
                 bot->GetName(),
                 DcSuppressionTransit::HoldName(
                     static_cast<DcSuppressionTransit::Hold>(st.transitHoldReason)),
                 cursor, in.trailDist, in.nearestEliteDist);

    st.transitHoldTimedOut = watchdogFired;
    st.transitHoldSinceMs = v.holdSinceMs;
    st.transitHoldReason = static_cast<uint8>(v.hold);

    // The cursor bump happens HERE, before publication, so the index and the
    // position it names can never disagree even for one tick.
    if (v.cursorAdvance && cursor < lastAnchor)
        ++cursor;
    WaypointHint const& aim = hints[cursor];

    // PUBLISH, on every branch that is not completion. Nothing on map 602 reads
    // this cross-bot today (the pack rung is gated to map 469), but the run-state
    // block is what `dc diag` prints and a crossing that logs its cursor is the
    // difference between a readable failure and a party standing in a pit.
    st.transitCursorIndex = cursor;
    st.transitCursorX = aim.x;
    st.transitCursorY = aim.y;
    st.transitCursorZ = aim.z;
    st.transitDrivingMs = now;

    if (!v.advance)
    {
        // HOLDING. Issue no movement and YIELD — a hold is exactly a tick on which
        // the party should be fighting rather than walking. Deliberately no
        // StopBot: killing the spline every tick of a Firestorm fight is a stop
        // packet every tick, and the bot is being held by combat anyway.
        return ObjectiveArriveResult::Done;
    }

    // WALKING. Through LongRangePathfinder, because a bare MovePoint truncates
    // silently past ~30yd and a straggling leader's catch-up leg is longer than
    // the authored 16. DcTransit::TravelTo returns false when the leader is
    // already inside the arrive radius, which the kernel has just told us it is
    // not — so a false here means the route could not be issued at all, and the
    // honest answer is to yield rather than claim a tick we did nothing with.
    if (!DcTransit::TravelTo(bot, botAI, aim.x, aim.y, aim.z, aim.arriveRadius))
        return ObjectiveArriveResult::Done;

    return ObjectiveArriveResult::Running;
}

// Id 24. The Violet Hold's are 15-19, Blackwing Lair's 20-21 and Halls of Stone's
// 22-23; ids are one flat space across every dungeon, and AddHook LOG_ERRORs a
// collision rather than silently dropping one.
void RegisterHallsOfLightningHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcHallsOfLightning::HOOK_SLAG_FURNACE_TRANSIT,
                                   &DriveSlagFurnaceTransit);
}
