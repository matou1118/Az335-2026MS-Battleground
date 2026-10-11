/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Trial of the Champion (map 650)
// anchors and for the places the driver takes the tank.
//
// The arena is one open bowl, so this is a short probe: every authored point
// snaps to the floor, and the entrance routes to each of them. The points that
// are not anchors are here because the driver or mod-playerbots walks to them —
// the announcer's three stands (the gossip reach is 4.5yd), the Black Knight's
// walk-to, the three soldier packs the driver pulls, and the four lance racks
// every bot has to reach before it can mount.
//
// It also PRINTS the routed polylines, deliberately: this is the tool the
// anchors were checked with.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, the same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='TrialOfTheChampionRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace DcTrialOfTheChampion;

    struct Pt { float x, y, z; char const* name; };

    // DcTestDungeonRegistry's map-650 row — where a walked-in party lands, 2.6yd
    // inside the North Portcullis.
    constexpr Pt ENTRANCE = { 805.23f, 618.04f, 412.39f, "entrance" };

    constexpr Pt OBJ1 = { CHAMPIONS_X, CHAMPIONS_Y, CHAMPIONS_Z, "OBJ1 the Grand Champions (joust post)" };
    constexpr Pt OBJ2 = { ARGENT_X, ARGENT_Y, ARGENT_Z, "OBJ2 the Argent Challenge" };
    constexpr Pt OBJ3 = { KNIGHT_X, KNIGHT_Y, KNIGHT_Z, "OBJ3 the Black Knight" };

    // The announcer's three stands, from instance_trial_of_the_champion: his spawn
    // and progress-0 home, his phase-2/3 stand (and every later cleanup's re-home),
    // and the side stand the on-foot cleanup parks him at.
    constexpr Pt ANNOUNCER_0  = { ARENA_X, ARENA_Y, ARENA_Z, "announcer (spawn, progress 0)" };
    constexpr Pt ANNOUNCER_6  = { 743.14f, 628.77f, 411.20f, "announcer (progress 6 / 8)" };
    constexpr Pt ANNOUNCER_SD = { 735.81f, 661.92f, 412.39f, "announcer (side stand)" };

    constexpr Pt KNIGHT_STAND = { 746.81f, 623.15f, 411.42f, "the Black Knight's walk-to" };

    // The three soldier packs' middle members (plan §1.6).
    constexpr Pt PACK_WEST   = { 717.86f, 649.00f, 412.0f, "soldier pack (west)" };
    constexpr Pt PACK_MIDDLE = { 746.73f, 650.24f, 412.0f, "soldier pack (middle)" };
    constexpr Pt PACK_EAST   = { 775.57f, 648.26f, 412.0f, "soldier pack (east)" };

    // The four Lance Racks (GO 196398), from the `gameobject` table.
    constexpr Pt RACKS[4] = {
        { 801.66f, 624.81f, 412.34f, "lance rack (entrance)" },
        { 784.53f, 660.24f, 412.39f, "lance rack (north-east)" },
        { 710.33f, 660.71f, 412.39f, "lance rack (north-west)" },
        { 692.13f, 610.58f, 412.35f, "lance rack (west)" },
    };

    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 6.0f;
    constexpr float SNAP_TOLERANCE = 3.0f;

    // The bowl is flat; anything taller than this between consecutive corridor
    // points is a lip or a drop, not the arena floor.
    constexpr float MAX_STEP_Z = 4.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 650";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-650 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    void SnapCheck(dtNavMesh const* mesh, Pt const& p)
    {
        G3D::Vector3 snapped;
        bool const ok = DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, SNAP_H, SNAP_V, snapped);
        float const d = ok ? std::sqrt((p.x - snapped.x) * (p.x - snapped.x) +
                                       (p.y - snapped.y) * (p.y - snapped.y) +
                                       (p.z - snapped.z) * (p.z - snapped.z))
                           : -1.0f;

        std::printf("  [%-38s] (%7.2f, %7.2f, %7.2f)  ->  ", p.name, p.x, p.y, p.z);
        if (ok)
            std::printf("(%7.2f, %7.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, d);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << p.name << " at (" << p.x << ", " << p.y << ", " << p.z
                        << ") is off the navmesh";
        if (ok)
            EXPECT_LT(d, SNAP_TOLERANCE) << p.name << " snapped " << d << "yd from where it was authored";
    }

    DcNavHarness::RouteResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult r = DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("\n=== Trial of the Champion (650): %s -> %s ===\n", a.name, b.name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                    r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                    r.maxStepZ, r.failureReason.c_str());
        for (std::size_t i = 0; i < r.points.size(); ++i)
            std::printf("  [pt %3zu] %8.2ff, %8.2ff, %7.2ff\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z);
        return r;
    }

    void ExpectRoutes(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r = PrintLeg(mesh, a, b);
        EXPECT_TRUE(r.reachable) << a.name << " -> " << b.name << " does not route: " << r.failureReason;
        EXPECT_TRUE(r.corridorComplete)
            << a.name << " -> " << b.name << " routes but stops short of the target poly";
        EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
            << a.name << " -> " << b.name << " has a " << r.maxStepZ << "yd vertical step";
    }

    std::vector<Pt> AllPoints()
    {
        std::vector<Pt> pts = { ENTRANCE, OBJ1, OBJ2, OBJ3, ANNOUNCER_0, ANNOUNCER_6, ANNOUNCER_SD,
                                KNIGHT_STAND, PACK_WEST, PACK_MIDDLE, PACK_EAST };
        for (Pt const& r : RACKS)
            pts.push_back(r);
        return pts;
    }
}

// Every anchor and every place the driver or the joust takes a bot is floor.
TEST(TrialOfTheChampionRouteProbe, EveryAuthoredPointIsOnTheNavmesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Trial of the Champion (650): authored points on the mesh ===\n");
    for (Pt const& p : AllPoints())
        SnapCheck(mesh.get(), p);
}

// The entrance reaches all of it: the three anchors in run order, the announcer's
// stands, the Knight's stand, the soldier packs and the four racks.
TEST(TrialOfTheChampionRouteProbe, TheEntranceRoutesToEveryAnchorAndStand)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    // The run's own legs, in order.
    ExpectRoutes(mesh.get(), ENTRANCE, OBJ1);
    ExpectRoutes(mesh.get(), OBJ1, OBJ2);
    ExpectRoutes(mesh.get(), OBJ2, OBJ3);

    // Everything else from the entrance (AllPoints()[0] is the entrance itself).
    std::vector<Pt> const pts = AllPoints();
    for (std::size_t i = 1; i < pts.size(); ++i)
        ExpectRoutes(mesh.get(), ENTRANCE, pts[i]);
}
