/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Navmesh probe for the authored Karazhan (map 532) routes to Attumen and to
// Moroes (the Moroes row is at the bottom of the file).
//
// Midnight's pen is a walled circle that opens only to the west. The corridor
// from the entrance runs along its east wall, 18-19yd from Midnight in a straight
// line, which is inside the at-boss handoff range, so the engage fired through
// the wall (tr-20260923-183454-1). The row holds the handoff until the tank has
// come round to the west mouth; these tests pin the geometry that makes that true.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='KarazhanRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace DcKarazhan;

namespace
{
    // The "Karazhan, Main (Entrance)" areatrigger's landing (4131).
    constexpr float ENTRANCE[3] = { -11100.0f, -2003.98f, 49.89f };

    // Where the run engaged from: the corridor along the pen's east wall.
    constexpr float EAST_WALL[3] = { -11124.53f, -1948.53f, 50.53f };

    // A leg routes "straight" when Detour's walk is within this of the chord; a
    // leg that routes much longer has its two anchors on opposite sides of a wall.
    constexpr float DETOUR_RATIO = 1.15f;
    constexpr float DETOUR_SLACK = 1.0f;
    constexpr float MAX_LEG = 25.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkip()
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
            return nullptr;
        return DcNavHarness::LoadMap(dir, MAP);
    }

    std::vector<WaypointHint> const& Row()
    {
        std::vector<WaypointHint> const* route =
            DungeonClearRouteRegistry::Get(MAP, DUNGEON_DIFFICULTY_NORMAL, NPC_MIDNIGHT);
        EXPECT_NE(route, nullptr) << "no authored route to Midnight on map 532";
        static std::vector<WaypointHint> const empty;
        return route ? *route : empty;
    }

    DcNavHarness::RouteResult Leg(dtNavMesh const* mesh, float const* a, float const* b)
    {
        return DcNavHarness::Route(mesh, MAP, a[0], a[1], a[2], b[0], b[1], b[2]);
    }
}

// The authoring aid: the full Detour corridor the row is decimated from.
TEST(KarazhanRouteProbe, PrintsTheCorridor)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    float const midnight[3] = { MIDNIGHT_X, MIDNIGHT_Y, MIDNIGHT_Z };
    DcNavHarness::RouteResult const r = Leg(mesh.get(), ENTRANCE, midnight);
    std::printf("=== Karazhan (532) entrance -> Midnight corridor ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f %s\n", r.reachable,
                r.corridorComplete, r.pointCount, r.routeLength2d, r.failureReason.c_str());
    for (std::size_t i = 0; i < r.points.size(); ++i)
        std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                    i, r.points[i].x, r.points[i].y, r.points[i].z);
    EXPECT_TRUE(r.reachable && r.corridorComplete);
}

TEST(KarazhanRouteProbe, EveryAnchorSnapsAndEveryLegRoutesStraight)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    std::vector<WaypointHint> const& row = Row();
    ASSERT_GE(row.size(), 2u);

    // The row starts where the party stands when the leg begins.
    EXPECT_LT(std::hypot(row.front().x - ENTRANCE[0], row.front().y - ENTRANCE[1]), 6.0f);

    std::vector<std::vector<float>> pts;
    for (WaypointHint const& h : row)
        pts.push_back({ h.x, h.y, h.z });
    pts.push_back({ MIDNIGHT_X, MIDNIGHT_Y, MIDNIGHT_Z });

    std::printf("=== Karazhan (532) Attumen route — per-leg ===\n");
    for (std::size_t i = 0; i < row.size(); ++i)
    {
        G3D::Vector3 s;
        ASSERT_TRUE(DcNavHarness::NearestPoint(mesh.get(), row[i].x, row[i].y, row[i].z,
                                               2.0f, 4.0f, s))
            << "anchor " << i << " is off the navmesh";
        float const snap = std::hypot(s.x - row[i].x, s.y - row[i].y);
        EXPECT_LT(snap, 0.5f) << "anchor " << i << " snaps " << snap << "yd away";
        EXPECT_LT(std::fabs(s.z - row[i].z), 1.5f) << "anchor " << i << " snaps to another level";
    }

    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
    {
        DcNavHarness::RouteResult const r = Leg(mesh.get(), pts[i].data(), pts[i + 1].data());
        float const chord = std::hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]);
        std::printf("  [leg %zu->%zu] chord %5.1f routed %5.1f reachable=%d complete=%d\n",
                    i, i + 1, chord, r.routeLength2d, r.reachable, r.corridorComplete);
        EXPECT_TRUE(r.reachable && r.corridorComplete) << "leg " << i;
        EXPECT_LE(r.routeLength2d, chord * DETOUR_RATIO + DETOUR_SLACK)
            << "leg " << i << " crosses a wall: the follower walks this chord straight";
        EXPECT_LE(chord, MAX_LEG) << "leg " << i;
    }
}

// The row's reason to exist: the handoff happens at the west mouth, where the
// line to Midnight is open, and nowhere the wall stands between.
TEST(KarazhanRouteProbe, TheEngageStartsAtThePensWestMouth)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    std::vector<WaypointHint> const& row = Row();
    ASSERT_FALSE(row.empty());
    WaypointHint const& last = row.back();
    float const lastPt[3] = { last.x, last.y, last.z };
    float const midnight[3] = { MIDNIGHT_X, MIDNIGHT_Y, MIDNIGHT_Z };

    // West is +y.
    EXPECT_GT(last.y - MIDNIGHT_Y, 12.0f) << "the last anchor is not on the pen's west side";

    // Inside the handoff range, so the engage takes over at the last anchor...
    float const straight = std::hypot(last.x - MIDNIGHT_X, last.y - MIDNIGHT_Y);
    EXPECT_LT(straight, DC_ENGAGE_RANGE);

    // ...and on the open side: the walk in is the straight line.
    DcNavHarness::RouteResult const r = Leg(mesh.get(), lastPt, midnight);
    std::printf("  [west mouth] straight %.1f routed %.1f\n", straight, r.routeLength2d);
    EXPECT_LE(r.routeLength2d, straight * DETOUR_RATIO + DETOUR_SLACK);

    // Control: the east wall, where tr-20260923-183454-1 engaged from, is inside
    // the handoff range in a straight line and far outside it on foot. If this
    // stops holding, the mesh changed and the row needs re-deriving.
    float const eastStraight = std::hypot(EAST_WALL[0] - MIDNIGHT_X, EAST_WALL[1] - MIDNIGHT_Y);
    DcNavHarness::RouteResult const east = Leg(mesh.get(), EAST_WALL, midnight);
    std::printf("  [east wall]  straight %.1f routed %.1f\n", eastStraight, east.routeLength2d);
    EXPECT_LT(eastStraight, DC_ENGAGE_RANGE);
    EXPECT_GT(east.routeLength2d, 3.0f * eastStraight);
}

// --- Attumen -> Moroes ------------------------------------------------------
//
// Detour's shortest walk from the stables to Moroes climbs the stair at the
// stables' north end into the Servants' Quarters; players go back out to the
// entrance and up the east stairs. The row holds the tank to the player route.

namespace
{
    std::vector<WaypointHint> const& MoroesRow()
    {
        std::vector<WaypointHint> const* route =
            DungeonClearRouteRegistry::Get(MAP, DUNGEON_DIFFICULTY_NORMAL, NPC_MOROES);
        EXPECT_NE(route, nullptr) << "no authored route to Moroes on map 532";
        static std::vector<WaypointHint> const empty;
        return route ? *route : empty;
    }

    // The follower flies an anchored leg as a straight 3D chord, so on the
    // stairs the chord must not pass under the floor.
    constexpr float CHORD_SAMPLE = 1.0f;
    constexpr float CHORD_MAX_UNDER = 1.0f;
}

TEST(KarazhanRouteProbe, PrintsTheMoroesCorridors)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    float const midnight[3] = { MIDNIGHT_X, MIDNIGHT_Y, MIDNIGHT_Z };
    float const moroes[3] = { MOROES_X, MOROES_Y, MOROES_Z };
    auto print = [&](char const* name, float const* a, float const* b)
    {
        DcNavHarness::RouteResult const r = Leg(mesh.get(), a, b);
        std::printf("=== Karazhan (532) %s ===\n", name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f %s\n", r.reachable,
                    r.corridorComplete, r.pointCount, r.routeLength2d, r.failureReason.c_str());
        for (std::size_t i = 0; i < r.points.size(); ++i)
            std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z);
        EXPECT_TRUE(r.reachable && r.corridorComplete) << name;
    };
    print("Midnight -> entrance", midnight, ENTRANCE);
    print("entrance -> Moroes", ENTRANCE, moroes);
}

TEST(KarazhanRouteProbe, MoroesRowSnapsRoutesStraightAndStaysOnTheStairs)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    std::vector<WaypointHint> const& row = MoroesRow();
    ASSERT_GE(row.size(), 2u);

    // The row starts where the party stands when Attumen dies.
    EXPECT_LT(std::hypot(row.front().x - MIDNIGHT_X, row.front().y - MIDNIGHT_Y), 6.0f);

    // Snapped the way the runtime snaps them: that Z is what flies.
    std::vector<G3D::Vector3> pts;
    for (std::size_t i = 0; i < row.size(); ++i)
    {
        G3D::Vector3 s;
        ASSERT_TRUE(DcNavHarness::NearestPoint(mesh.get(), row[i].x, row[i].y, row[i].z,
                                               2.0f, 4.0f, s))
            << "anchor " << i << " is off the navmesh";
        float const snap = std::hypot(s.x - row[i].x, s.y - row[i].y);
        EXPECT_LT(snap, 0.5f) << "anchor " << i << " snaps " << snap << "yd away";
        EXPECT_LT(std::fabs(s.z - row[i].z), 1.5f) << "anchor " << i << " snaps to another level";
        pts.push_back(s);
    }
    pts.push_back(G3D::Vector3(MOROES_X, MOROES_Y, MOROES_Z));

    std::printf("=== Karazhan (532) Moroes route — per-leg ===\n");
    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
    {
        G3D::Vector3 const& a = pts[i];
        G3D::Vector3 const& b = pts[i + 1];
        float const pa[3] = { a.x, a.y, a.z };
        float const pb[3] = { b.x, b.y, b.z };
        DcNavHarness::RouteResult const r = Leg(mesh.get(), pa, pb);
        float const chord = std::hypot(b.x - a.x, b.y - a.y);

        float worstUnder = 0.0f;
        int const steps = std::max(1, static_cast<int>(chord / CHORD_SAMPLE));
        for (int k = 1; k < steps; ++k)
        {
            G3D::Vector3 const c = a + (b - a) * (static_cast<float>(k) / steps);
            G3D::Vector3 s;
            if (DcNavHarness::NearestPoint(mesh.get(), c.x, c.y, c.z, 1.0f, 3.0f, s))
                worstUnder = std::max(worstUnder, s.z - c.z);
        }

        std::printf("  [leg %2zu->%2zu] chord %5.1f routed %5.1f under %4.2f reachable=%d complete=%d\n",
                    i, i + 1, chord, r.routeLength2d, worstUnder, r.reachable, r.corridorComplete);
        EXPECT_TRUE(r.reachable && r.corridorComplete) << "leg " << i;
        EXPECT_LE(r.routeLength2d, chord * DETOUR_RATIO + DETOUR_SLACK)
            << "leg " << i << " crosses a wall: the follower walks this chord straight";
        EXPECT_LE(chord, MAX_LEG) << "leg " << i;
        EXPECT_LT(worstUnder, CHORD_MAX_UNDER) << "leg " << i << " cuts under the floor";
    }

    // The last anchor hands off to the engage: inside its range.
    EXPECT_LT(std::hypot(row.back().x - MOROES_X, row.back().y - MOROES_Y), DC_ENGAGE_RANGE);
}

// The row's reason to exist. If Detour's walk from the stables stops going
// through the Servants' Quarters (z ~75 above the stables), the mesh changed
// and the row needs re-deriving.
TEST(KarazhanRouteProbe, DetourTakesTheServantsQuartersToMoroes)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    float const pen[3] = { WEST_STANDOFF_X, WEST_STANDOFF_Y, WEST_STANDOFF_Z };
    float const moroes[3] = { MOROES_X, MOROES_Y, MOROES_Z };
    DcNavHarness::RouteResult const direct = Leg(mesh.get(), pen, moroes);
    ASSERT_TRUE(direct.reachable && direct.corridorComplete);

    bool viaServants = false;
    for (G3D::Vector3 const& p : direct.points)
        if (p.x < -11090.0f && p.z > 70.0f)
            viaServants = true;

    std::vector<WaypointHint> const& row = MoroesRow();
    float rowLen = 0.0f;
    for (std::size_t i = 0; i + 1 < row.size(); ++i)
        rowLen += std::hypot(row[i + 1].x - row[i].x, row[i + 1].y - row[i].y);

    std::printf("  [direct] %.1fyd via servants=%d, row %.1fyd\n",
                direct.routeLength2d, viaServants, rowLen);
    EXPECT_TRUE(viaServants);
    EXPECT_LT(direct.routeLength2d, rowLen);
}

// tr-20260927-190943-12: the tank dropped off the Opera balcony onto the audience
// floor mid-fight, under a route cursor 14yd overhead. A fresh route built from
// the floor must still reach the urn (it climbs back via the balcony stair);
// that is what the off-level re-path in the rejoin ladder relies on.
TEST(KarazhanRouteProbe, OperaFloorRepathsToTheUrn)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    float const floors[2][3] = {
        { -10935.6f, -1835.7f, 95.3f },   // where the fight left the tank
        { -10925.3f, -1858.7f, 96.1f },   // where it stood at teardown
    };
    float const urn[3] = { URN_X, URN_Y, URN_Z };
    for (auto const& f : floors)
    {
        DcNavHarness::RouteResult const r = Leg(mesh.get(), f, urn);
        float topZ = f[2];
        for (G3D::Vector3 const& p : r.points)
            topZ = std::max(topZ, p.z);
        std::printf("  [floor->urn] from (%.1f,%.1f,%.1f): reachable=%d complete=%d %.1fyd, %u pts, "
                    "max z %.1f, end (%.1f,%.1f,%.1f) %s\n",
                    f[0], f[1], f[2], r.reachable, r.corridorComplete, r.routeLength2d,
                    r.pointCount, topZ,
                    r.points.empty() ? 0.0f : r.points.back().x,
                    r.points.empty() ? 0.0f : r.points.back().y,
                    r.points.empty() ? 0.0f : r.points.back().z, r.failureReason.c_str());
        EXPECT_TRUE(r.reachable && r.corridorComplete);
    }
}

// tr-20260927-201144-3: from the ramp above the Homunculus room the pull tag leg
// aimed at a point on the straight line to the pack, at the pack's height. That
// point is in the gap between ramp and room: no floor under it until ~53yd down.
// Resolved on its own level (what the snap in the Advancing branch does) it is a
// short walk toward the pack; resolved to the floor below (what stock
// SearchForBestPath's GetMapHeight picked) it is a ~580yd route whose first legs
// climb the ramp away from the pack — the loop the run showed.
TEST(KarazhanRouteProbe, HomunculusTagAimPointRoutesOnlyOnItsOwnLevel)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 532";

    float const commit[3] = { -11187.1f, -1693.9f, 183.3f };   // route pt 182
    float const pack[2] = { -11212.8f, -1688.3f };
    float const aimOwnLevel[3] = { -11195.2f, -1692.1f, 179.3f };
    float const aimFloorBelow[3] = { -11195.2f, -1692.1f, 125.9f };

    auto farthestFromPack = [&](DcNavHarness::RouteResult const& r)
    {
        float d = 0.0f;
        for (G3D::Vector3 const& p : r.points)
            d = std::max(d, std::hypot(p.x - pack[0], p.y - pack[1]));
        return d;
    };
    float const start = std::hypot(commit[0] - pack[0], commit[1] - pack[1]);

    DcNavHarness::RouteResult const own = Leg(mesh.get(), commit, aimOwnLevel);
    DcNavHarness::RouteResult const below = Leg(mesh.get(), commit, aimFloorBelow);
    std::printf("  [own level] %.1fyd, farthest from pack %.1f (start %.1f)\n",
                own.routeLength2d, farthestFromPack(own), start);
    std::printf("  [floor below] %.1fyd, farthest from pack %.1f\n",
                below.routeLength2d, farthestFromPack(below));

    ASSERT_TRUE(own.reachable);
    EXPECT_LT(own.routeLength2d, 15.0f);
    EXPECT_LE(farthestFromPack(own), start + 0.5f);
    EXPECT_GT(below.routeLength2d, 200.0f);
    EXPECT_GT(farthestFromPack(below), 45.0f);
}
