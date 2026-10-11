/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCNOSTOPZONE_H
#define _PLAYERBOT_DCNOSTOPZONE_H

#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/WaypointHint.h"

class AiObjectContext;
class Player;

// "May the party set up a pull where it is standing?"
//
// Some stretches of an authored route are places a party must CROSS, not places
// it may camp — and the reason is geometry the pull system cannot see. The case
// this was built for is Blackwing Lair's Vaelastrasz->staging hall: it runs
// 24.3yd directly UNDERNEATH the upper suppression room, and 24% of its length
// is inside the 3D aggro radius of the ~70 Technicians, Warlocks, Overseers and
// Spellbinders standing on the floor above. They aggro through the floor, cannot
// path down, and evade where they stand; nothing the party does there can
// resolve them, and every second spent standing still buys another wave.
//
// The pull system makes it strictly worse rather than merely slow. A legitimate
// same-floor pack sits at the far end of that hall, and `safe-camp: target is a
// ranged attacker -> requiring LOS break, maxDrag extended to 60yd` drags the
// camp BACKWARD along the route — into the middle of the overhead band, which is
// then where the party fights, loots, and rests. On tp-20260828-175353-1 all five
// raids ended their run inside it.
//
// So the route says so. An anchor flagged AnchorFlag::NO_STOP declares that the
// LEG LEAVING IT must be crossed without a pull, and this predicate answers
// whether a bot is currently standing on such a leg. Authored rather than
// inferred on purpose: the hazard is a floor overhead, and a scan cheap enough to
// run every tick cannot tell "trash above the ceiling" from "trash up the ramp
// we are about to walk". The route author already knows.
//
// Deliberately NOT a combat suppression. Anything that reaches the party in the
// zone is still fought where it stands — the party is walking, not fleeing, and
// the tank still holds what lands on it. What stands down is the PLANNING: no
// camp, no drag, no LOS-break setback, no scout-lag stranding. Same "one switch
// instead of per-mechanic suppressions" shape as DungeonEvent::ownsThePull, which
// it shares its wiring with (DungeonClearPullModeCurrentValue).
namespace DcNoStopZone
{
    // Is this bot standing on a NO_STOP leg of its party's active authored route?
    //
    // Resolves the route the same way Advance does — (map, difficulty, the current
    // DcKey::NextDungeonBoss entry) — so it follows the party from objective to
    // objective with no state of its own, and answers false for every dungeon that
    // has not authored a span. Memoised per tick (DcTickMemo): several rungs ask it
    // each tick, and at raid sizes an unmemoised answer is a fresh route walk every
    // time.
    bool IsInNoStopZone(Player* bot, AiObjectContext* context);

    // --- the pure half, so a test can pin a route's span against real coordinates
    //     without a world ---------------------------------------------------

    // Squared 3D distance from (px,py,pz) to the segment a-b. The zone is a
    // CAPSULE around the leg, not a sphere around its anchors: anchors on the BWL
    // approach sit 11-20yd apart, so anchor-radius tests would leave unprotected
    // holes in the middle of exactly the stretch that needs covering.
    float DistSqToSegment(float px, float py, float pz,
                          WaypointHint const& a, WaypointHint const& b);

    // Does (px,py,pz) lie within `radius` of any leg LEAVING a NO_STOP-flagged
    // anchor of `route`? A flag on anchor i covers the leg i -> i+1, so the last
    // anchor can never open a span — right, because it is a destination, not a leg.
    bool CoversPoint(std::vector<WaypointHint> const& route,
                     float px, float py, float pz, float radius);
}

#endif  // _PLAYERBOT_DCNOSTOPZONE_H
