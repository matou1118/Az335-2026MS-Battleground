/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"

namespace
{
    // Beyond this a bare MovePoint is not trusted to deliver — the silent
    // 74-poly truncation. Same number and same reason as the Violet Hold's
    // VH_LONG_HAUL and the orb runner's ORB_LONG_HAUL.
    constexpr float TRANSIT_LONG_HAUL = 30.0f;

    // Floor between two spline issues for the SAME destination. Out of combat the
    // escort-generator test below carries it alone; in combat it does not, because
    // MoveChase is layered back over the escort slot whenever the stock engine
    // wins a tick, and without a time floor this would rebuild a route every tick
    // for the whole crossing.
    constexpr uint32 TRANSIT_REISSUE_MS = 1500;

    // "Is the glide in flight still aimed at the right place?" — a CEILING, not a
    // constant. The cursor moves by one authored leg at a time (4-24yd), so a flat
    // epsilon wide enough to absorb route slop on a long leg would swallow a short
    // one entirely and the driver would issue nothing, for ever. Scaling it to
    // half the remaining trip (floor 2yd) makes the test narrower than the
    // movement it gates, always.
    constexpr float TRANSIT_REPATH_EPSILON = 10.0f;

    float RepathEpsilon(float dist)
    {
        return std::min(TRANSIT_REPATH_EPSILON, std::max(2.0f, dist * 0.5f));
    }

    // Below this the bot has no usable bearing from the anchor (it is effectively
    // standing on it), so there is no "near edge" to aim at. The caller gets the
    // anchor itself and the next tick, from a real bearing, does the holding.
    constexpr float BEARING_FLOOR = 1.0f;
}

bool DcTransit::TravelTo(Player* bot, PlayerbotAI* botAI, float x, float y, float z, float leash,
                         bool forcePath)
{
    if (!bot || !botAI)
        return false;

    float const dist = bot->GetExactDist(x, y, z);
    if (dist <= leash)
        return false;  // arrived — the caller must NOT own the tick

    float const epsilon = RepathEpsilon(dist);

    // An escort glide already in flight owns the bot. Let it finish if it is
    // headed here; drop it if it is stale — a route to the PREVIOUS cursor would
    // otherwise ride all the way out before the bot could react to the leg the
    // leader has actually moved on to.
    MotionMaster* mm = bot->GetMotionMaster();
    float dx, dy, dz;
    if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
        mm->GetDestination(dx, dy, dz))
    {
        float const ex = dx - x, ey = dy - y, ez = dz - z;
        if (std::sqrt(ex * ex + ey * ey + ez * ez) <= epsilon)
            return true;  // gliding here already — let it ride, keep the tick
        DcMovement::ResolveEscortConflict(bot);
    }

    // Same-destination re-issue floor: the test above cannot see a glide the
    // combat engine has since layered MoveChase over.
    //
    // Kept on the BOT'S OWN run state — see Util/DcThrottle.h for why not in a
    // file-scope thread_local map keyed by GUID.
    if (DcRun::Of(botAI).ThrottledIssue(DcThrottle::TransitIssue, x, y, z,
                                       epsilon, TRANSIT_REISSUE_MS))
        return true;

    if (forcePath || dist > TRANSIT_LONG_HAUL)
    {
        ChunkedPathfinder::Result const path = LongRangePathfinder::Build(bot, x, y, z);
        if (path.reachable && !path.segments.empty())
        {
            // Element 0 is the live position — the escort path[0]=start convention
            // SplinePath expects.
            Movement::PointsArray points;
            points.push_back(
                G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
            for (PathSegment const& seg : path.segments)
            {
                // A jump leg cannot be expressed as a ground spline. The certified
                // corridor has a maximum vertical step of 2.6yd, so this should
                // never fire; deliver what we have if it does.
                if (seg.jumpDown || seg.jumpGap)
                    break;
                for (G3D::Vector3 const& p : seg.polyline)
                {
                    if (points.size() >= DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS)
                        break;
                    points.push_back(p);
                }
            }
            if (DcMovement::SplinePath(botAI, points))
                return true;
        }
        // No navmesh route: fall through. MovePoint will not deliver at this range
        // either, but it is a better last word than standing still.
    }

    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                      /*speed*/ 0.0f, /*orientation*/ 0.0f,
                                      /*generatePath*/ true, /*forceDestination*/ false);
    return true;
}

namespace
{
    // One built view per (map, boss, slice). Registry rows are seeded once and
    // never mutated, so a view is immutable after construction and the map only
    // ever grows — but it IS looked up from more than one rung, so the build is
    // guarded rather than reasoned about.
    struct RouteKey
    {
        uint32 mapId;
        uint32 bossEntry;
        std::size_t first;
        std::size_t last;

        bool operator==(RouteKey const& o) const
        {
            return mapId == o.mapId && bossEntry == o.bossEntry && first == o.first &&
                   last == o.last;
        }
    };

    struct RouteKeyHash
    {
        std::size_t operator()(RouteKey const& k) const noexcept
        {
            std::size_t h = std::hash<uint32>{}(k.mapId);
            h = h * 31 + std::hash<uint32>{}(k.bossEntry);
            h = h * 31 + std::hash<std::size_t>{}(k.first);
            h = h * 31 + std::hash<std::size_t>{}(k.last);
            return h;
        }
    };
}

DcTransit::RouteView const* DcTransit::Route(uint32 mapId, uint32 bossEntry,
                                             std::size_t firstAnchor, std::size_t lastAnchor)
{
    static std::mutex mtx;
    // unique_ptr so a rehash never moves a view a caller is already holding.
    static std::unordered_map<RouteKey, std::unique_ptr<RouteView>, RouteKeyHash> cache;

    RouteKey const key{ mapId, bossEntry, firstAnchor, lastAnchor };

    std::lock_guard<std::mutex> guard(mtx);
    auto const it = cache.find(key);
    if (it != cache.end())
        return it->second ? it->second.get() : nullptr;

    std::unique_ptr<RouteView> built;
    std::vector<WaypointHint> const* row =
        DungeonClearRouteRegistry::Get(mapId, DUNGEON_DIFFICULTY_NORMAL, bossEntry);

    // A slice needs at least two anchors to be a route: one to stand on and one
    // to walk to. Anything shorter is a row that has been edited out from under
    // the driver, and the honest answer is nullptr — the driver then reports Done
    // and the leg goes back to being ordinary.
    if (row && firstAnchor + 1 < row->size())
    {
        std::size_t const last = std::min(lastAnchor, row->size() - 1);
        if (last > firstAnchor)
        {
            built = std::make_unique<RouteView>();
            built->hints.assign(row->begin() + firstAnchor, row->begin() + last + 1);
            built->anchors.reserve(built->hints.size());
            for (WaypointHint const& h : built->hints)
                built->anchors.push_back({ h.x, h.y, h.z });
        }
    }

    RouteView const* const out = built.get();
    cache.emplace(key, std::move(built));
    return out;
}

DcTransit::HoldTarget DcTransit::HoldPoint(Player* bot, Position const& anchor, float leash,
                                           float margin, float snapRadius, float snapTolerance,
                                           RouteView const* route)
{
    HoldTarget out;
    out.x = anchor.GetPositionX();
    out.y = anchor.GetPositionY();
    out.z = anchor.GetPositionZ();
    if (!bot)
        return out;

    float const back = leash - margin;

    // Never further out than the bot already is: the hold point pulls a bot IN,
    // and a bot inside the ring must not be pushed out to sit on it. z rides the
    // same fraction — see the header, and DungeonClearMath::PointTowardFrom.
    Position const chord =
        DungeonClearMath::PointTowardFrom(anchor, bot->GetPosition(), back, BEARING_FLOOR);

    // Is the chord standing on the corridor, or in the middle of the C?
    NavmeshSnap::Result const snap =
        NavmeshSnap::Snap(bot, chord.GetPositionX(), chord.GetPositionY(),
                          chord.GetPositionZ(), snapRadius);
    if (snap.ok && snap.distance <= snapTolerance)
    {
        // Take the SNAPPED point, not the raw chord. The two are within tolerance
        // of each other by construction here, and the snapped one is on a polygon
        // the pathfinder can name.
        out.x = snap.x;
        out.y = snap.y;
        out.z = snap.z;
        return out;
    }

    // The chord is off the floor. Ride the authored polyline instead: one leash
    // back along the corridor from the cursor, which is walkable ground by
    // certification (the per-map route probe suites).
    if (!route)
    {
        // No row to fall back to. The chord is still a better last word than
        // standing still, and TravelTo's pathfinder gets to refuse it honestly.
        out.x = chord.GetPositionX();
        out.y = chord.GetPositionY();
        out.z = chord.GetPositionZ();
        return out;
    }

    uint32 const cursor = DcSuppressionTransit::AnchorIndexOf(
        route->anchors, anchor.GetPositionX(), anchor.GetPositionY(), anchor.GetPositionZ());
    DcSuppressionTransit::Anchor const behind =
        DcSuppressionTransit::PointBehindOnRoute(route->anchors, cursor, back);

    out.x = behind.x;
    out.y = behind.y;
    out.z = behind.z;
    out.viaRoute = true;
    return out;
}
