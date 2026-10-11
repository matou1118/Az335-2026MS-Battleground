/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Blackwing Lair (map 469) route
// through the Suppression Rooms, Vaelastrasz -> Broodlord Lashlayer.
//
// WHY THIS IS A GATE AND NOT A NICETY. The route was NOT read off a live client:
// it was reconstructed from the map-469 mmtiles (poly verts + `neis` links, tile
// borders matched by portal-edge overlap, seams bridged by a centroid stitch),
// and a stitch of that kind can invent a link across a railing. An anchor a
// yard inside a wall is a transit that walks the raid into geometry with 160
// whelps on a 30s respawn behind it. So every hint is snapped against the REAL
// Detour mesh here, and the corridor is routed end to end with the same
// LongRangePathfinder core the runtime uses.
//
// The suite also PRINTS the routed polyline. That is deliberate: this is the
// tool the hints were authored with (route the leg, decimate the polyline), so
// an mmaps regen that moves the corridor can be re-authored the same way instead
// of by hand.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, same contract as the Azjol-Nerub,
// Mechanar and Ramparts probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='BlackwingLairSuppressionRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace DcBlackwingLair;

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this anchor is not standing anywhere near here", not "something was found
    // across the room"; vertical covers the ordinary float between an authored z
    // and the mesh surface under it without reaching the OTHER suppression room,
    // whose floor is 9yd above the lower one's.
    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 4.0f;

    // How far the mesh may move an authored anchor before it stops being an
    // authored anchor. A couple of yards is ordinary detail-mesh float; more than
    // that and the point was written somewhere the party cannot stand.
    constexpr float SNAP_TOLERANCE = 2.5f;

    // The routed corridor's length, pinned in a band. The measured 2D length of
    // the staging -> standoff crossing is ~375yd; a regen that reroutes the leg
    // (through the other room, or around the ramp) moves this by tens of yards
    // and must trip red rather than silently changing what the transit crosses.
    constexpr float ROUTE_MIN_2D = 300.0f;
    constexpr float ROUTE_MAX_2D = 470.0f;

    // Largest vertical step between consecutive corridor points that is still a
    // WALK. The whole leg is ramps: 437 -> 446 through the lower room, 446 -> 449
    // over the Taskmaster ramp. Anything above this is a ledge the raid would
    // have to fall down, and this route has none.
    constexpr float MAX_STEP_Z = 6.0f;

    // The rally point the raid actually stands on when Vaelastrasz dies (measured
    // off tr-20260828-111233-4 @ 11:21:36) and the head of the authored approach.
    // The row has to start here, not at the staging point: the 317yd between them
    // is what four of five raids could not walk in tp-20260828-111227-1.
    constexpr float VAEL_RALLY_X = -7506.70f;
    constexpr float VAEL_RALLY_Y = -1014.10f;
    constexpr float VAEL_RALLY_Z = 408.69f;

    // The approach's routed 2D length, pinned in a band for the same reason the
    // crossing's is: a regen that reroutes it moves this by tens of yards and must
    // trip red rather than silently changing where the raid walks.
    constexpr float APPROACH_MIN_2D = 260.0f;
    constexpr float APPROACH_MAX_2D = 380.0f;

    // The authored track, in route order — anchor 0 is the Vaelastrasz rally, the
    // anchor at TRANSIT_STAGE_ANCHOR_INDEX IS the staging point and the last one
    // IS the standoff (see RegisterBlackwingLairRoute), so this is the row itself
    // rather than the row with its ends bolted on.
    std::vector<G3D::Vector3> FullPolyline(std::vector<WaypointHint> const& hints)
    {
        std::vector<G3D::Vector3> out;
        out.reserve(hints.size());
        for (WaypointHint const& h : hints)
            out.emplace_back(h.x, h.y, h.z);
        return out;
    }

    // Longest single leg the transit is allowed to issue. The driver walks one
    // leg per hop through LongRangePathfinder, so a long leg is legal — but a leg
    // longer than this is one the pack leash cannot hold a raid across.
    constexpr float MAX_LEG_2D = 30.0f;

    // TransitPackLeash's registry default. Named here rather than read through
    // DcSettings because this suite has no bot to read a per-run override off —
    // the geometry question is about the shipped default.
    constexpr float TRANSIT_PACK_LEASH_DEFAULT = 25.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 469";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-469 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    std::vector<WaypointHint> const* Route()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL,
                                              NPC_BROODLORD_LASHLAYER);
    }
}

// Every authored anchor — and both fixed ends — must be somewhere a bot can
// actually stand. This is the assertion the reconstruction warning is about.
TEST(BlackwingLairSuppressionRouteProbe, EveryAnchorIsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr) << "no authored route to Broodlord Lashlayer on map 469";
    ASSERT_FALSE(route->empty());

    std::vector<G3D::Vector3> const line = FullPolyline(*route);

    // THREE contractual points, not two. The transit's staging hold, its
    // completion test, the corridor bbox and the pack cursor all name the last
    // two; the first is what gives the clear a polyline to project onto while it
    // is still standing on Vaelastrasz's corpse.
    ASSERT_GT(line.size(), TRANSIT_STAGE_ANCHOR_INDEX + 1);
    EXPECT_NEAR(line.front().x, VAEL_RALLY_X, 0.01f);
    EXPECT_NEAR(line.front().y, VAEL_RALLY_Y, 0.01f);
    EXPECT_NEAR(line.front().z, VAEL_RALLY_Z, 0.01f);
    // BwlTransitRoute slices the row here and hands the driver the tail. If this
    // index stops naming the staging point the driver's cursor 0 — the gather
    // gate, the arm, the pack anchor — silently means somewhere else.
    G3D::Vector3 const& stage = line[TRANSIT_STAGE_ANCHOR_INDEX];
    EXPECT_NEAR(stage.x, TRANSIT_STAGE_X, 0.01f)
        << "TRANSIT_STAGE_ANCHOR_INDEX no longer names the staging point";
    EXPECT_NEAR(stage.y, TRANSIT_STAGE_Y, 0.01f);
    EXPECT_NEAR(stage.z, TRANSIT_STAGE_Z, 0.01f);
    EXPECT_NEAR(line.back().x, TRANSIT_END_X, 0.01f);
    EXPECT_NEAR(line.back().y, TRANSIT_END_Y, 0.01f);
    EXPECT_NEAR(line.back().z, TRANSIT_END_Z, 0.01f);

    std::printf("=== Blackwing Lair (469) suppression route — anchor snap ===\n");
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        G3D::Vector3 const& p = line[i];
        char const* what = (i == 0) ? "rally"
                         : (i == TRANSIT_STAGE_ANCHOR_INDEX) ? "staging"
                         : (i + 1 == line.size()) ? "standoff"
                         : (i < TRANSIT_STAGE_ANCHOR_INDEX) ? "approach"
                                                            : "anchor";

        G3D::Vector3 snapped;
        bool const ok =
            DcNavHarness::NearestPoint(mesh.get(), p.x, p.y, p.z, SNAP_H, SNAP_V, snapped);
        float const delta =
            ok ? std::sqrt((snapped.x - p.x) * (snapped.x - p.x) +
                           (snapped.y - p.y) * (snapped.y - p.y) +
                           (snapped.z - p.z) * (snapped.z - p.z))
               : -1.0f;

        std::printf("  [%-8s %2zu] (%9.2f, %9.2f, %7.2f)  ->  ", what, i, p.x, p.y, p.z);
        if (ok)
            std::printf("(%9.2f, %9.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, delta);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << what << " " << i << " at (" << p.x << ", " << p.y << ", " << p.z
                        << ") is off the navmesh — the transit would walk the raid into geometry";
        if (ok)
            EXPECT_LT(delta, SNAP_TOLERANCE)
                << what << " " << i << " snapped " << delta
                << "yd — it is not standing where it was authored";
    }
    std::printf("============================================================\n");
}

// The corridor bbox is the transit's real activation gate — one integer compare
// away from being the only thing between a rung on every bot's combat engine and
// the other seven encounters of this raid. If a CROSSING anchor falls outside it
// the driver goes inert halfway across the gauntlet.
//
// The APPROACH anchors are the other way round: they are the walk up to the
// gauntlet, so an approach anchor inside the box arms the transit EARLY. That is
// only tolerable within TRANSIT_STAGE_SKIP_DIST of the staging point, where an
// early arm still gathers the raid at staging (the driver's own skip test) — the
// last hop onto the staging shelf is legitimately inside the box and there is no
// honest way to draw the box around the shelf without it. Any FURTHER back and
// the arm latches the gather open ("already past the staging point"), which is
// exactly the 86yd arm that cost tr-20260828-111233-4 the leg.
TEST(BlackwingLairSuppressionRouteProbe, EveryAnchorIsInsideTheCorridorBox)
{
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_GT(route->size(), TRANSIT_STAGE_ANCHOR_INDEX + 1);

    auto inBox = [](WaypointHint const& h)
    {
        return h.x >= TRANSIT_BOX_MIN_X && h.x <= TRANSIT_BOX_MAX_X &&
               h.y >= TRANSIT_BOX_MIN_Y && h.y <= TRANSIT_BOX_MAX_Y &&
               h.z >= TRANSIT_BOX_MIN_Z && h.z <= TRANSIT_BOX_MAX_Z;
    };

    for (std::size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        if (i < TRANSIT_STAGE_ANCHOR_INDEX)
        {
            float const toStage = std::sqrt(
                (h.x - TRANSIT_STAGE_X) * (h.x - TRANSIT_STAGE_X) +
                (h.y - TRANSIT_STAGE_Y) * (h.y - TRANSIT_STAGE_Y) +
                (h.z - TRANSIT_STAGE_Z) * (h.z - TRANSIT_STAGE_Z));
            EXPECT_TRUE(!inBox(h) || toStage < TRANSIT_STAGE_SKIP_DIST)
                << "approach anchor " << i << " is inside the transit corridor "
                << toStage << "yd from staging — the driver would arm there and "
                   "SKIP the gather";
        }
        else
            EXPECT_TRUE(inBox(h)) << "crossing anchor " << i;
    }

    // ...and the box must NOT swallow the two encounters behind it, or the
    // transit arms in Razorgore's chamber and in Vaelastrasz's room.
    auto inside = [](float x, float y, float z)
    {
        return x >= TRANSIT_BOX_MIN_X && x <= TRANSIT_BOX_MAX_X &&
               y >= TRANSIT_BOX_MIN_Y && y <= TRANSIT_BOX_MAX_Y &&
               z >= TRANSIT_BOX_MIN_Z && z <= TRANSIT_BOX_MAX_Z;
    };
    EXPECT_FALSE(inside(VAEL_X, VAEL_Y, VAEL_Z)) << "Vaelastrasz is inside the transit corridor";
    EXPECT_FALSE(inside(ORB_X, ORB_Y, ORB_Z)) << "the Orb of Domination is inside the transit corridor";
    EXPECT_FALSE(inside(CAMP_X, CAMP_Y, CAMP_Z)) << "the Razorgore camp is inside the transit corridor";
    EXPECT_FALSE(inside(GRETHOK_X, GRETHOK_Y, GRETHOK_Z)) << "Grethok is inside the transit corridor";
}

// The crossing has to be one continuous corridor from the staging point to the
// Broodlord standoff, with no ledge in it and a length that has not moved.
TEST(BlackwingLairSuppressionRouteProbe, TheStagingPointRoutesToTheStandoff)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const r =
        DcNavHarness::Route(mesh.get(), MAP_ID,
                            TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z,
                            TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z);

    std::printf("=== Blackwing Lair (469) suppression corridor ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                r.maxStepZ, r.failureReason.c_str());
    // The polyline itself — this is the authoring tool (see the file header).
    for (std::size_t i = 0; i < r.points.size(); ++i)
        std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                    i, r.points[i].x, r.points[i].y, r.points[i].z);
    std::printf("=================================================\n");

    EXPECT_TRUE(r.reachable)
        << "the staging point cannot path to the Broodlord standoff at all";
    EXPECT_TRUE(r.corridorComplete)
        << "the corridor stops short of the standoff — the transit would end in the gauntlet";
    EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
        << "the corridor contains a " << r.maxStepZ << "yd vertical step — that is a ledge, "
           "and this leg has none";
    EXPECT_GT(r.routeLength2d, ROUTE_MIN_2D);
    EXPECT_LT(r.routeLength2d, ROUTE_MAX_2D)
        << "the corridor has been rerouted (" << r.routeLength2d
        << "yd) — re-author the hints from the polyline printed above";
}

// THE APPROACH, certified end to end. This is the leg that was missing entirely
// until S2043: from where the raid stands when Vaelastrasz dies to the staging
// point, 317yd through a switchback that climbs 15yd. It has to be one continuous
// corridor with no ledge in it, or the clear is back to rejoining a route it
// cannot reach.
TEST(BlackwingLairSuppressionRouteProbe, TheVaelastraszRallyRoutesToTheStagingPoint)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const r =
        DcNavHarness::Route(mesh.get(), MAP_ID,
                            VAEL_RALLY_X, VAEL_RALLY_Y, VAEL_RALLY_Z,
                            TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z);

    std::printf("=== Blackwing Lair (469) approach corridor ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                r.maxStepZ, r.failureReason.c_str());
    // The polyline itself — this is the authoring tool (see the file header).
    for (std::size_t i = 0; i < r.points.size(); ++i)
        std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                    i, r.points[i].x, r.points[i].y, r.points[i].z);
    std::printf("==============================================\n");

    EXPECT_TRUE(r.reachable)
        << "the Vaelastrasz rally cannot path to the staging point at all";
    EXPECT_TRUE(r.corridorComplete)
        << "the approach stops short of staging — the clear would rejoin a route "
           "it cannot reach, which is the S2043 failure verbatim";
    EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
        << "the approach contains a " << r.maxStepZ << "yd vertical step — that is a ledge";
    EXPECT_GT(r.routeLength2d, APPROACH_MIN_2D);
    EXPECT_LT(r.routeLength2d, APPROACH_MAX_2D)
        << "the approach has been rerouted (" << r.routeLength2d
        << "yd) — re-author anchors 0-" << (TRANSIT_STAGE_ANCHOR_INDEX - 1)
        << " from the polyline printed above";
}

// The whole row, start to finish. Two certified halves that do not JOIN are still
// two routes: the clear walks this as one polyline and a gap at the staging point
// is a rebuild in the worst possible place.
TEST(BlackwingLairSuppressionRouteProbe, TheRallyRoutesAllTheWayToTheStandoff)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const r =
        DcNavHarness::Route(mesh.get(), MAP_ID,
                            VAEL_RALLY_X, VAEL_RALLY_Y, VAEL_RALLY_Z,
                            TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z);

    std::printf("=== Blackwing Lair (469) rally -> standoff ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                r.maxStepZ, r.failureReason.c_str());
    std::printf("==============================================\n");

    EXPECT_TRUE(r.reachable);
    EXPECT_TRUE(r.corridorComplete);
    EXPECT_LT(r.maxStepZ, MAX_STEP_Z);
    EXPECT_GT(r.routeLength2d, APPROACH_MIN_2D + ROUTE_MIN_2D);
    EXPECT_LT(r.routeLength2d, APPROACH_MAX_2D + ROUTE_MAX_2D);
}

// Consecutive anchors must be close enough that the runtime can walk one leg at
// a time. The transit issues each leg through LongRangePathfinder, so a leg is
// allowed to be long — but a leg the follower cannot resnap over is how a
// mid-route rebuild sends the party back through the room it just crossed.
TEST(BlackwingLairSuppressionRouteProbe, EveryLegIsWalkable)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_FALSE(route->empty());

    std::vector<G3D::Vector3> const line = FullPolyline(*route);
    float total = 0.0f;

    std::printf("=== Blackwing Lair (469) suppression route — per-leg ===\n");
    for (std::size_t i = 1; i < line.size(); ++i)
    {
        G3D::Vector3 const& a = line[i - 1];
        G3D::Vector3 const& b = line[i];
        float const leg = std::hypot(b.x - a.x, b.y - a.y);
        total += leg;

        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("  [leg %2zu] %6.1fyd  reachable=%d complete=%d routed=%6.1f %s\n",
                    i, leg, r.reachable, r.corridorComplete, r.routeLength2d,
                    r.failureReason.c_str());

        EXPECT_TRUE(r.reachable) << "leg " << i << " is not walkable";
        EXPECT_TRUE(r.corridorComplete) << "leg " << i << " does not reach its anchor";
        // A leg whose routed length far exceeds the straight line is a leg that
        // goes AROUND something — i.e. two anchors on opposite sides of a wall,
        // which is exactly what the centroid stitch can invent.
        EXPECT_LT(r.routeLength2d, leg * 1.6f + 12.0f)
            << "leg " << i << " routes " << r.routeLength2d << "yd for a " << leg
            << "yd hop — the two anchors are not on the same side of the geometry";
        EXPECT_LT(leg, MAX_LEG_2D)
            << "leg " << i << " is " << leg << "yd — too long for the pack leash to hold";
    }
    std::printf("  total authored polyline: %.1fyd\n", total);
    std::printf("========================================================\n");
}

// THE HOLD POINT, certified against the same mesh as the anchors.
//
// The transit's pack rung positions every follower relative to the leader's
// cursor. It used to do that with a bare CHORD — the straight-line point one
// leash from the cursor, z interpolated along the same fraction — handed
// straight to MovePoint. A chord only describes real ground when the leg is
// straight, and the climb out of the staging shelf is not: the walkable floor
// bows ~7yd east around a hole, so the chord from a follower behind it sinks
// under the ramp or into the void. tr-20260830-125018-2 is 1109 executions of
// that rung, 95.2% of which handed MovePoint a destination off the floor; the
// raid clipped the ramp.
//
// DcTransit::HoldPoint now snaps the chord and, when the snap cannot find
// walkable ground near it, falls back to the point one leash back along the
// AUTHORED POLYLINE. This measures both rungs of that ladder on the real mesh,
// for followers at two different places behind the same cursor, and pins the
// property that actually matters: the destination must be WALKABLE TO from where
// the follower is standing, not merely on a polygon somewhere.
TEST(BlackwingLairSuppressionRouteProbe, TheHoldPointIsReachableFromBehindTheClimb)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_GT(route->size(), TRANSIT_STAGE_ANCHOR_INDEX + 4);

    // The transit slice, as DcTransit::Route() builds it.
    std::vector<DcSuppressionTransit::Anchor> anchors;
    for (std::size_t i = TRANSIT_STAGE_ANCHOR_INDEX; i < route->size(); ++i)
        anchors.push_back({ (*route)[i].x, (*route)[i].y, (*route)[i].z });

    constexpr uint32 CURSOR = 3;  // the lower room, one leg past the bulge
    constexpr float BACK = TRANSIT_PACK_LEASH_DEFAULT - TRANSIT_PACK_HOLD_MARGIN;
    DcSuppressionTransit::Anchor const& cursor = anchors[CURSOR];

    struct Follower
    {
        char const* where;
        float x, y, z;
    };
    // Both behind the same cursor, on ground the mesh agrees is walkable.
    std::vector<Follower> const followers = {
        { "north arm", -7630.0f, -920.0f, 439.7f },
        { "staging",   TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z },
    };

    // The corridor fallback has to be LOAD-BEARING for at least one of them, or
    // it is dead code dressed as a safety net.
    int fellBackToTheCorridor = 0;

    std::printf("=== Blackwing Lair (469) climb — pack hold point ===\n");
    for (Follower const& f : followers)
    {
        float const dx = f.x - cursor.x, dy = f.y - cursor.y;
        float const bearing = std::hypot(dx, dy);
        ASSERT_GT(bearing, BACK) << f.where << " is inside the leash; no hold to take";

        float const frac = BACK / bearing;
        float const chordX = cursor.x + dx * frac;
        float const chordY = cursor.y + dy * frac;
        float const chordZ = cursor.z + (f.z - cursor.z) * frac;

        G3D::Vector3 snapped;
        bool const onMesh = DcNavHarness::NearestPoint(mesh.get(), chordX, chordY, chordZ,
                                                       TRANSIT_HOLD_SNAP_RADIUS,
                                                       TRANSIT_HOLD_SNAP_V_EXTENT, snapped);
        float const miss =
            onMesh ? std::sqrt((snapped.x - chordX) * (snapped.x - chordX) +
                               (snapped.y - chordY) * (snapped.y - chordY) +
                               (snapped.z - chordZ) * (snapped.z - chordZ))
                   : -1.0f;

        // The production rule, spelled out: take the snapped chord when the mesh
        // barely had to move it, else ride the polyline.
        bool const viaRoute = !onMesh || miss > TRANSIT_HOLD_SNAP_TOLERANCE;
        if (viaRoute)
            ++fellBackToTheCorridor;

        DcSuppressionTransit::Anchor const corridor =
            DcSuppressionTransit::PointBehindOnRoute(anchors, CURSOR, BACK);
        float const hx = viaRoute ? corridor.x : snapped.x;
        float const hy = viaRoute ? corridor.y : snapped.y;
        float const hz = viaRoute ? corridor.z : snapped.z;

        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), MAP_ID, f.x, f.y, f.z, hx, hy, hz);

        std::printf("  [%-9s] chord (%.1f, %.1f, %.1f) onMesh=%d miss=%5.2f -> %s\n",
                    f.where, chordX, chordY, chordZ, onMesh, miss,
                    viaRoute ? "CORRIDOR" : "snapped chord");
        std::printf("              hold  (%.1f, %.1f, %.1f) reachable=%d complete=%d "
                    "routed=%.1fyd (straight %.1fyd) %s\n",
                    hx, hy, hz, r.reachable, r.corridorComplete, r.routeLength2d,
                    std::hypot(hx - f.x, hy - f.y), r.failureReason.c_str());

        EXPECT_TRUE(r.reachable) << f.where << ": the hold point cannot be walked to";
        EXPECT_TRUE(r.corridorComplete) << f.where << ": the route stops short of the hold point";
    }

    EXPECT_GT(fellBackToTheCorridor, 0)
        << "no follower needed the corridor fallback — the C has been re-meshed, and "
           "DcTransit::HoldPoint's fallback is now dead code";
    std::printf("========================================================\n");
}
