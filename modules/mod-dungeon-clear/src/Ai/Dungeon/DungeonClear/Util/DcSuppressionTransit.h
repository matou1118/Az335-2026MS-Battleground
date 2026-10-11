/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSUPPRESSIONTRANSIT_H
#define _PLAYERBOT_DCSUPPRESSIONTRANSIT_H

#include <cstddef>
#include <vector>

#include "Position.h"

#include "Ai/Dungeon/DungeonClear/Data/WaypointHint.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"

class Player;
class PlayerbotAI;

// The movement half of a TRANSIT — the two things a leader's driver and (where
// there is one) every follower's pack rung both need, shared so the two halves
// can never disagree about them.
//
// Written for Blackwing Lair's Suppression Rooms and now also used by Halls of
// Lightning's Slag Furnace, which is why the route slice and the hold point's
// snap box are parameters rather than constants read out of DcBlackwingLair.
// Nothing here knows about either map.
//
// It is a namespace of free functions rather than a class because there is no
// state here: both are pure geometry plus a spline issue, and the decision that
// USES them is the pure kernel in DcSuppressionTransitDecision.h.
namespace DcTransit
{
    // Walk `bot` to (x, y, z) unless it is already within `leash` of it.
    //
    // Long-haul aware and SAFE TO CALL EVERY TICK IN COMBAT, which is the whole
    // reason it exists as its own function. Three things have to be true at once
    // on this leg and none of them is free:
    //
    //   * A bare MovePoint past ~30yd fails SILENTLY — the engine PathGenerator
    //     caps a generated path at 74 polys / 74 points and truncates, leaving the
    //     bot standing still with no failure to observe at the call site. The
    //     authored legs run to 24yd and a straggler's catch-up is longer, so the
    //     long haul goes through LongRangePathfinder and a real spline.
    //   * There is NO `if (bot->isMoving()) return;` guard. In combat a bot is
    //     essentially always moving under MoveChase, so such a guard makes the
    //     whole transit a no-op for the entire crossing.
    //   * ...which means the re-issue floor is what stops it becoming spline spam,
    //     because the stock combat engine layers MoveChase back over the escort
    //     slot every tick it wins.
    //
    // `forcePath` makes the long-haul branch unconditional. The 30yd gate below is
    // a proxy for "is the walk long enough to truncate", and a proxy that reads
    // the STRAIGHT-LINE distance is wrong exactly where the ground is not
    // straight: around Blackwing Lair's C-shaped staging climb the corridor runs
    // 45yd between two points 16yd apart. A caller that already knows its
    // destination is around a bend says so here rather than letting the proxy
    // decide.
    //
    // Returns true when it issued (or is riding) movement, so the caller can own
    // the tick; false when the bot is already inside the leash.
    bool TravelTo(Player* bot, PlayerbotAI* botAI, float x, float y, float z, float leash,
                  bool forcePath = false);

    // Where to walk a bot the pack wants back: the point on the leash boundary
    // around `anchor` nearest to the bot, pulled `margin` INSIDE it — the near
    // EDGE, never the centre.
    //
    // Aiming at the centre is what the Razorgore camp's first cut did and what
    // CAMP_HOLD_MARGIN exists to prevent: a bot crosses the whole camp inward, the
    // fight pushes it back out, and it crosses again, forever, every bot out of
    // phase with the others. It matters MORE here, because this anchor advances
    // toward the bot as well — aiming at the centre of a moving leash makes every
    // correction a sprint past the leader instead of a step back into formation.
    //
    // Bearing is taken in 2D; z is INTERPOLATED along the same fraction. This used
    // to take the anchor's height outright, on the reasoning that the route is
    // authored on the floor the party walks — but the hold point is NOT the
    // anchor. It sits a leash back along the bearing, typically within a few yards
    // of the bot, so the anchor's floor height there describes a point that exists
    // nowhere the moment the leg is not flat; the crossing's ramps climb up to 5yd
    // over a single 20yd leg. Same defect the heal-reposition fallback carried, and
    // the same fix — see DungeonClearMath::PointTowardFrom.
    //
    // A follower genuinely off the route's floor (mid-fall, a tier up) is not
    // rescued by either choice: that is stranded recovery's rung, not this one.
    //
    // ...AND THE BEARING IS THEN CHECKED AGAINST THE MESH, because interpolating z
    // along a chord only describes real ground when the leg is straight. The climb
    // out of the staging shelf is a C: the walkable surface bows seven yards east
    // around a hole, so the chord from a follower on the north arm to a cursor on
    // the south arm crosses the void and its interpolated z lands 2.3yd under the
    // only floor there. Live, tr-20260830-125018-2, that is 95% of this rung's
    // executions walking at a destination inside the rock.
    //
    // So: take the chord, snap it, and if the snap cannot find walkable ground
    // near it, fall back to the point one leash BACK ALONG THE AUTHORED POLYLINE
    // (`viaRoute`). The chord is kept for the ordinary straight leg because the
    // ring it produces is what SPREADS the raid by bearing; the corridor fallback
    // is a deliberate collapse to single file, which is the right formation for a
    // ramp anyway.
    struct HoldTarget
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        bool viaRoute{false};  // the chord was off-mesh; this rides the corridor
    };

    // The transit's authored track, SLICED so that anchor 0 is the crossing's
    // staging point and the last anchor is the point the crossing ends at —
    // SHARED, so the leader's cursor and every follower's hold point are read off
    // one row. Both halves used to be able to disagree about the route; now they
    // cannot.
    //
    // The slice matters to the KERNEL, not just to tidiness: DcSuppressionTransit
    // keys its gather gate on `cursorIndex == 0`, its arm pins the cursor to 0,
    // ResolveCursor clamps against a stored index in the same space, and the
    // completion test is "the LAST anchor". Hand a driver the unsliced row and
    // every one of those means something else.
    //
    // `firstAnchor`/`lastAnchor` are INCLUSIVE indices into the registry row, and
    // `lastAnchor` past the end clamps. Blackwing Lair takes a TAIL slice (its
    // crossing ends at the next boss, so the row simply stops there); Halls of
    // Lightning takes a MIDDLE one (the party still has a hairpin and a second
    // ramp to walk after its crossing, and those must not be steered by a driver
    // whose route ends behind them).
    //
    // nullptr when the registry row is missing or too short to slice. Views are
    // built once per (map, boss, slice) and cached; the cache is mutex-guarded
    // because a route is looked up from the leader's driver and from every
    // follower's pack rung, and there is no reason to reason about which thread
    // won.
    struct RouteView
    {
        std::vector<WaypointHint> hints;                    // the slice, in route order
        std::vector<DcSuppressionTransit::Anchor> anchors;  // ...the same, projected
    };

    RouteView const* Route(uint32 mapId, uint32 bossEntry, std::size_t firstAnchor,
                           std::size_t lastAnchor);

    HoldTarget HoldPoint(Player* bot, Position const& anchor, float leash, float margin,
                         float snapRadius, float snapTolerance, RouteView const* route);
}

#endif  // _PLAYERBOT_DCSUPPRESSIONTRANSIT_H
