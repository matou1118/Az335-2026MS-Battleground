/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Log.h"

#include <cstdint>
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"

// Blackwing Lair's Suppression Rooms — the RAID's half of the transit.
//
// The leader's half (the driver, Overrides/BlackwingLairDriver.cpp) walks one
// authored leg at a time and publishes the anchor it is walking to. This is what
// every other member does about that: stay inside one leash of it.
//
// IT IS A MOVING CAMP, and that is the only thing that distinguishes it from the
// Razorgore camp one room over. There is no point to stand at on this leg — only
// a 342yd line to walk — so the camp advances with the leader, and the walk-back
// aims at the near EDGE of a leash that is itself closing on the bot.
//
// WHY IT IS LOAD-BEARING RATHER THAN COSMETIC. Two rooms, 160 whelps, a 30s
// respawn. A raid strung over 100yd sweeps a far larger cylinder of that than a
// raid in a 25yd ball, and — this is the part that ends runs — every straggler
// independently holds the WHOLE party in combat, because AnyPartyEngagement is
// party-wide. DcCombatFlag::MayDrive is false while anything is engaged, and
// Advance lives only in the non-combat engine, so a strung-out raid is a raid
// whose clear has no driver at all. Travelling as one body is the precondition
// for travelling.

namespace
{
    using namespace DcBlackwingLair;
}

bool DungeonClearTransitPackTrigger::IsActive()
{
    // Map first: this is registered in both engines on every bot, and everywhere
    // outside Blackwing Lair it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != MAP_ID)
        return false;

    // The leader IS the anchor. Exempting it here rather than inside the signal
    // keeps GetTransitAnchor answerable by the driver itself.
    //
    // Resolved ONCE and handed on. This used to call FindLeaderTank and then
    // GetTransitAnchor, which calls it again — two acquisitions of the one
    // process-wide leader-election mutex, per bot, per engine, per tick, on the
    // map where forty of them are ticking.
    Player* const leader = DcLeaderSignal::FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;

    // One call answers both "is there a crossing in progress" and "where is its
    // cursor" — and it is false the moment the driver stops stamping, so this
    // rung releases within two AI ticks of the crossing ending by every exit the
    // leg has.
    Position anchor;
    if (!DcLeaderSignal::GetTransitAnchorFrom(leader, anchor))
        return false;

    // INERT inside the HOLD POINT's radius — never "own the tick and return
    // false". A bot already in the pack must not contend with its own rotation
    // for the whole crossing; the fine positioning inside the pack stays the raid
    // strategy's.
    //
    // AND THE RELEASE RADIUS IS THE HOLD POINT'S, NOT THE LEASH'S. This used to
    // release at `> leash` while Execute walked to `leash - margin`, which reads
    // like hysteresis and is the exact opposite of it: the rung went inert the
    // instant the bot crossed the leash inward, MoveChase layered straight back
    // over it, and the bot drifted out again. Measured over
    // tp-20260828-121941-1's five runs, 6167 executions of this rung landed in
    // [25.00, 26.00) yards of a 25yd leash and 76 landed inside it — the whole
    // raid ratcheting on the boundary, which pinned the driver's pack hold for
    // 87-91% of its ticks. Reading the same radius the walk-back AIMS at is what
    // makes the margin a real band instead of a decoration.
    float const leash = DcSettings::GetFloat(bot, "TransitPackLeash");
    return bot->GetExactDist(anchor) > leash - TRANSIT_PACK_HOLD_MARGIN;
}

bool DungeonClearTransitPackAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    Position anchor;
    if (!DcLeaderSignal::GetTransitAnchor(bot, anchor))
        return false;  // raced away between the trigger and here

    float const leash = DcSettings::GetFloat(bot, "TransitPackLeash");

    // The snap box, the tolerance and the polyline are Blackwing Lair's — this
    // rung is BWL-gated on its first line (see the trigger), and the helper is
    // shared with the Halls of Lightning driver, which passes its own.
    DcTransit::HoldTarget const hold =
        DcTransit::HoldPoint(bot, anchor, leash, TRANSIT_PACK_HOLD_MARGIN,
                             TRANSIT_HOLD_SNAP_RADIUS, TRANSIT_HOLD_SNAP_TOLERANCE,
                             DcTransit::Route(MAP_ID, NPC_BROODLORD_LASHLAYER,
                                              TRANSIT_STAGE_ANCHOR_INDEX, SIZE_MAX));

    // The arrival leash on the walk-back is deliberately tight — the hold point
    // is already inside the pack leash by TRANSIT_PACK_HOLD_MARGIN, so a loose
    // one here would hand the tick back while the bot is still outside the leash
    // the trigger measures and re-fire on the very next tick.
    //
    // `viaRoute` FORCES the validated pathfinder. TravelTo's own 30yd gate reads
    // the straight-line distance, and the hold point is only ever `leash - margin`
    // nearer the cursor than the bot already is — so this rung would need a bot
    // 51yd off the cursor before the gate ever opened, and 95.2% of its executions
    // in tr-20260830-125018-2 (1056 of 1109) fell through to a bare MovePoint.
    // That is survivable on straight ground and is exactly wrong on the C, where
    // the corridor runs 45yd between points 16yd apart. When HoldPoint has told us
    // it had to ride the polyline, we already know the ground is not straight.
    if (!DcTransit::TravelTo(bot, botAI, hold.x, hold.y, hold.z,
                             /*leash*/ TRANSIT_PACK_ARRIVE_LEASH,
                             /*forcePath*/ hold.viaRoute))
        return false;

    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] BWL transit — closing on the pack ({:.1f}yd off the "
              "cursor, leash {:.0f}){}",
              bot->GetName(), bot->GetExactDist(anchor), leash,
              hold.viaRoute ? " -> chord off-mesh, riding the corridor" : "");
    return true;
}
