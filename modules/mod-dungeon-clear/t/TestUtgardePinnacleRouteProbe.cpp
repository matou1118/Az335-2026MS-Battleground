/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Route-derivation probe AND CERTIFIER for Utgarde Pinnacle (map 575).
//
// It began as a printer — the tool that answered the questions below before a
// single anchor was written — and it is now also the GATE on the six legs that
// were written from its output. Both halves belong in one file: the polylines it
// prints are where the authored anchors came from, so an mmaps regen that moves a
// corridor fails the certification tests here and is re-authored from the print
// in the same run. See [[dc-navharness-prints-the-route]].
//
// The certification tests live at the bottom; the questions it was built to
// answer are:
//
//   * where does the party actually walk, entrance -> each boss;
//   * whether the two shut portcullises (192173, 192174) sit ON those corridors
//     or merely near them — the run tr-20260902-083808-1 stalled at
//     (481.4, -458.6, 104.7) on "a closed door is blocking the path" with no
//     door in front of the party, and 192173 is 19yd away;
//   * whether Svala's arena (z 90.6) is on a different navmesh storey from the
//     main floor (z ~104.8), which is what decides whether boss 1 needs a
//     transit leg of its own.
//
// The navmesh is baked DOOR-BLIND: a shut portcullis is a runtime GameObject and
// leaves no hole in the mesh. So a corridor printed here is the corridor DC will
// try to walk THROUGH a closed door, which is exactly what makes the door
// question answerable offline.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, same contract as the Azjol-Nerub,
// Blackwing Lair, Halls of Lightning, Mechanar and Ramparts probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='UtgardePinnacleRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t MAP_UTGARDE_PINNACLE = 575;

    struct Pt { float x, y, z; char const* name; };

    // DcTestDungeonRegistry's map-575 row — the areatrigger_teleport target a
    // walked-in party lands on.
    constexpr Pt ENTRANCE = { 584.12f, -327.97f, 110.14f, "entrance" };

    // Live `creature` rows. Svala is entry 29281 (the INTRO npc that transforms);
    // 26668 Svala Sorrowgrave, the encounter's credit entry, has no spawn row.
    constexpr Pt SVALA    = { 296.60f, -346.10f,  90.60f, "Svala (29281)" };
    constexpr Pt PALEHOOF = { 320.80f, -453.10f, 104.80f, "Gortok Palehoof" };
    constexpr Pt SKADI    = { 343.00f, -507.30f, 104.60f, "Skadi the Ruthless" };
    constexpr Pt YMIRON   = { 392.80f, -286.80f, 109.30f, "King Ymiron" };

    // Live `gameobject` rows. Both spawn GO_STATE_READY (shut) and both are
    // lock-free script doors, so a bot is not entitled to force either.
    constexpr Pt DOOR_ENTRY = { 445.10f, -325.50f, 101.00f, "GO 192174 portcullis+chain" };
    constexpr Pt DOOR_SKADI = { 477.50f, -477.20f, 103.10f, "GO 192173 portcullis" };

    // Where tr-20260902-083808-1 died: paused on a door, target Palehoof 159.2yd.
    constexpr Pt STALL = { 481.40f, -458.60f, 104.70f, "tr-...-1 stall point" };

    // Harpoon Launcher 192175 — the head of Skadi's gauntlet balcony.
    constexpr Pt HARPOON = { 491.50f, -508.20f, 105.90f, "Harpoon Launcher 192175" };

    // The two SmartTrigger areatriggers that START bosses 1 and 3. The test
    // harness relays both (they carry areatrigger_scripts rows and no teleport
    // row), so a route that brushes either one fires that encounter.
    constexpr Pt AT_SVALA = { 312.65f, -291.17f, 104.70f, "AT 5140 Svala start" };
    constexpr Pt AT_SKADI = { 330.90f, -508.43f, 104.27f, "AT 4991 Skadi gauntlet" };

    // GO 188593 Stasis Generator — the ONLY way to start Gortok Palehoof.
    constexpr Pt STASIS   = { 238.52f, -460.83f, 105.48f, "GO 188593 Stasis Generator" };

    // Skadi phase-2 landing (spell 61790) and the gauntlet add spawn corner.
    constexpr Pt SKADI_P2 = { 476.80f, -511.17f, 104.72f, "Skadi phase-2 landing" };
    constexpr Pt ADD_SPAWN= { 477.58f, -484.56f, 104.82f, "Skadi add spawn" };

    constexpr float SNAP_H = 6.0f;
    constexpr float SNAP_V = 8.0f;

    void ReportSnap(dtNavMesh const* mesh, Pt const& p)
    {
        G3D::Vector3 out;
        if (!DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, SNAP_H, SNAP_V, out))
        {
            std::printf("  [snap] %-28s (%8.2f, %8.2f, %7.2f)  OFF-MESH within %.0f/%.0f\n",
                        p.name, p.x, p.y, p.z, SNAP_H, SNAP_V);
            return;
        }
        std::printf("  [snap] %-28s (%8.2f, %8.2f, %7.2f) -> (%8.2f, %8.2f, %7.2f)  dz=%+6.2f d2d=%5.2f\n",
                    p.name, p.x, p.y, p.z, out.x, out.y, out.z,
                    out.z - p.z, std::hypot(out.x - p.x, out.y - p.y));
    }

    // Closest approach of a routed corridor to a point, in 2D and in 3D, plus the
    // index of the nearest polyline vertex. This is what says whether a door is
    // ON the corridor or merely NEAR it.
    void ReportProximity(DcNavHarness::RouteResult const& r, Pt const& p)
    {
        if (r.points.empty())
        {
            std::printf("      (no polyline; cannot measure %s)\n", p.name);
            return;
        }
        float best2d = 1e9f;
        float best3d = 1e9f;
        size_t bestIdx = 0;
        float alongAtBest = 0.0f;
        float along = 0.0f;
        for (size_t i = 0; i < r.points.size(); ++i)
        {
            if (i)
                along += std::hypot(r.points[i].x - r.points[i - 1].x,
                                    r.points[i].y - r.points[i - 1].y);
            float const d2 = std::hypot(r.points[i].x - p.x, r.points[i].y - p.y);
            if (d2 < best2d)
            {
                best2d = d2;
                best3d = std::sqrt(d2 * d2 + (r.points[i].z - p.z) * (r.points[i].z - p.z));
                bestIdx = i;
                alongAtBest = along;
            }
        }
        std::printf("      %-28s nearest vertex %3zu/%3zu  2d=%7.2f  3d=%7.2f  %6.1fyd along\n",
                    p.name, bestIdx, r.points.size(), best2d, best3d, alongAtBest);
    }

    void PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b, bool printPolyline)
    {
        DcNavHarness::RouteResult const r = DcNavHarness::Route(
            mesh, MAP_UTGARDE_PINNACLE, a.x, a.y, a.z, b.x, b.y, b.z);

        float const straight = std::hypot(b.x - a.x, b.y - a.y);
        std::printf("\n  [leg] %s -> %s\n", a.name, b.name);
        std::printf("      reachable=%d complete=%d startFar=%d pts=%u len2d=%.1f (straight %.1f, x%.2f) maxStepZ=%.2f %s\n",
                    r.reachable, r.corridorComplete, r.startFarFromPoly,
                    r.pointCount, r.routeLength2d, straight,
                    straight > 0.1f ? r.routeLength2d / straight : 0.0f,
                    r.maxStepZ, r.failureReason.c_str());

        ReportProximity(r, DOOR_ENTRY);
        ReportProximity(r, DOOR_SKADI);

        if (!printPolyline || r.points.empty())
            return;

        std::printf("      polyline (%zu pts):\n", r.points.size());
        float along = 0.0f;
        for (size_t i = 0; i < r.points.size(); ++i)
        {
            if (i)
                along += std::hypot(r.points[i].x - r.points[i - 1].x,
                                    r.points[i].y - r.points[i - 1].y);
            std::printf("        %3zu  (%8.2f, %8.2f, %7.2f)  %7.1fyd\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z, along);
        }
    }

    dtNavMesh const* g_mesh = nullptr;

    std::shared_ptr<dtNavMesh> Load(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 575";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_UTGARDE_PINNACLE);
        if (!mesh)
            why = std::string("no map-575 navmesh under ") + dir + "/mmaps";
        return mesh;
    }
}

TEST(UtgardePinnacleRouteProbe, EveryFixedPointStandsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Utgarde Pinnacle (575) fixed-point snap ===\n");
    for (Pt const& p : { ENTRANCE, SVALA, PALEHOOF, SKADI, YMIRON,
                         DOOR_ENTRY, DOOR_SKADI, STALL, HARPOON,
                         AT_SVALA, AT_SKADI, STASIS, SKADI_P2, ADD_SPAWN })
        ReportSnap(mesh.get(), p);
    std::printf("==============================================\n");

    G3D::Vector3 out;
    EXPECT_TRUE(DcNavHarness::NearestPoint(mesh.get(), ENTRANCE.x, ENTRANCE.y, ENTRANCE.z,
                                           SNAP_H, SNAP_V, out))
        << "the map-575 test-registry entrance is off the navmesh";
}

TEST(UtgardePinnacleRouteProbe, PrintsEveryBossLeg)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Utgarde Pinnacle (575) boss legs ===\n");

    // The clear's own order question: the roster DC derives today is
    // Palehoof -> Skadi -> Ymiron (Svala is missing, no spawn row for 26668).
    PrintLeg(mesh.get(), ENTRANCE, PALEHOOF, true);
    PrintLeg(mesh.get(), PALEHOOF, SKADI,    true);
    PrintLeg(mesh.get(), SKADI,    YMIRON,   true);

    // The order the dungeon is actually built for.
    PrintLeg(mesh.get(), ENTRANCE, SVALA,    true);
    PrintLeg(mesh.get(), SVALA,    PALEHOOF, true);

    std::printf("\n=======================================\n");
}

TEST(UtgardePinnacleRouteProbe, TheDesignedProgressionLegs)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Utgarde Pinnacle (575) designed progression ===\n");

    // Boss 1 is started by walking into AT 5140, not by aggroing anything.
    PrintLeg(mesh.get(), ENTRANCE, AT_SVALA, true);
    PrintLeg(mesh.get(), AT_SVALA, SVALA,    true);

    // Boss 2 needs the Stasis Generator clicked, which is 84yd PAST Palehoof.
    PrintLeg(mesh.get(), SVALA,    STASIS,   true);
    PrintLeg(mesh.get(), STASIS,   PALEHOOF, true);

    // Boss 3's gauntlet is started by AT 4991; phase 2 lands 134yd east.
    PrintLeg(mesh.get(), PALEHOOF, AT_SKADI, true);
    PrintLeg(mesh.get(), AT_SKADI, HARPOON,  true);
    PrintLeg(mesh.get(), HARPOON,  SKADI_P2, false);

    // Boss 4 after Skadi opens 192173.
    PrintLeg(mesh.get(), SKADI_P2, YMIRON,   true);

    std::printf("\n==================================================\n");
}

TEST(UtgardePinnacleRouteProbe, WhatEachPortcullisActuallyGates)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Utgarde Pinnacle (575) door-gating probe ===\n");

    // King Ymiron is the last boss and 192174 sits 52yd off the Skadi->Ymiron
    // corridor, so it is NOT his room door. Route to him from every other fixed
    // point and see which portcullis, if either, is on the way.
    PrintLeg(mesh.get(), ENTRANCE, YMIRON,   true);
    PrintLeg(mesh.get(), SVALA,    YMIRON,   false);
    PrintLeg(mesh.get(), PALEHOOF, YMIRON,   true);

    // Skadi from each predecessor.
    PrintLeg(mesh.get(), ENTRANCE, SKADI,    false);
    PrintLeg(mesh.get(), SVALA,    SKADI,    false);

    // And the two ends of each door, to name the rooms it joins. A portcullis
    // blocks along its facing, so step 8yd either side of the GO.
    struct Side { Pt p; } sides[] = {
        {{ 453.10f, -325.50f, 104.92f, "192174 EAST side (entrance hall)" }},
        {{ 437.10f, -325.50f, 104.92f, "192174 WEST side" }},
        {{ 477.50f, -469.20f, 104.93f, "192173 NORTH side (stall room)" }},
        {{ 477.50f, -485.20f, 104.93f, "192173 SOUTH side (Skadi gauntlet)" }},
    };
    std::printf("\n  --- door sides ---\n");
    for (Side const& s : sides)
        ReportSnap(mesh.get(), s.p);

    std::printf("\n===============================================\n");
}

TEST(UtgardePinnacleRouteProbe, ReproducesTheStallPointCorridor)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== tr-20260902-083808-1 stall reproduction ===\n");

    // Where the party stood, and where DC was steering it. If the corridor from
    // here to Palehoof passes within a couple of yards of GO 192173 then the
    // door flag was CORRECT and the dungeon is genuinely gated; if it passes
    // wide, the flag was a false positive and the pause is a DC bug.
    PrintLeg(mesh.get(), STALL, PALEHOOF, true);

    // And the leg that got them there.
    PrintLeg(mesh.get(), ENTRANCE, STALL, false);

    // Is Skadi's balcony reachable at all with the mesh door-blind?
    PrintLeg(mesh.get(), STALL, HARPOON, false);

    std::printf("\n==============================================\n");
}

// ===========================================================================
// CERTIFICATION — the gate half.
//
// Everything above prints. Everything below asserts, against the SAME live
// mmtiles, that the six anchor rows authored from those prints still describe
// ground a party can walk. The two halves are one file on purpose: when a regen
// moves a corridor, the test that fails and the print that re-authors it are the
// same run.
// ===========================================================================

namespace
{
    using namespace DcUtgardePinnacle;

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this anchor is not standing anywhere near here" rather than "something was
    // found on the storey below" — which on a map with three overlapping floors
    // (the main ring at z ~105, Svala's arena at z ~87 and the lower ring at
    // z ~75) is a real risk and the whole reason the vertical box is tight too.
    constexpr float CERT_SNAP_H = 4.0f;
    constexpr float CERT_SNAP_V = 4.0f;

    // How far the mesh may move an authored anchor before it stops being an
    // authored anchor. A couple of yards is ordinary detail-mesh float; more than
    // that and the point was written somewhere the party cannot stand.
    constexpr float CERT_SNAP_TOLERANCE = 2.5f;

    // THE FLOOR TEST. The playable box runs z 75 -> 120 and the lowest ground the
    // party ever stands on is the lower ring at z 75.6. Anything under this is
    // under the dungeon — map 575 is NOT in the flat-grid-height family
    // ([[ac-map601-flat-gridheight-zero]]), and this assertion is what keeps that
    // true rather than assuming it.
    constexpr float CERT_FLOOR_Z = 70.0f;

    // Largest vertical step between consecutive CORRIDOR points that is still a
    // walk. The measured maximum across all six designed legs is 3.3yd. Anything
    // above this is a ledge the party would have to fall down, and none of these
    // legs has one.
    constexpr float CERT_MAX_STEP_Z = 6.0f;

    // How far the NAV_GROUND-only snap may drop below the permissive one before
    // the point counts as standing on liquid ([[dc-navmesh-liquid-test-ask-the-poly]]).
    constexpr float CERT_DRY_TOLERANCE = 2.0f;

    uint32 CertObj(uint32 seq) { return BossRosterRegistry::ObjectiveEntry(seq); }

    std::vector<WaypointHint> const* CertRoute(uint32 entry)
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL, entry);
    }

    // Vertical drop from the permissive snap to the NAV_GROUND-only snap, or a
    // large sentinel when no ground poly is in range at all. Both mean liquid.
    float CertGroundDrop(dtNavMesh const* mesh, float x, float y, float z)
    {
        G3D::Vector3 any;
        if (!DcNavHarness::NearestPoint(mesh, x, y, z, CERT_SNAP_H, 15.0f, any))
            return 1e9f;
        G3D::Vector3 ground;
        if (!DcNavHarness::NearestPoint(mesh, x, y, z, CERT_SNAP_H, 15.0f, ground, NAV_GROUND))
            return 1e9f;
        return std::fabs(any.z - ground.z);
    }

}

// Every authored anchor on every leg must be somewhere a bot can actually stand,
// and must be ABOVE the dungeon's own floor.
TEST(UtgardePinnacleRouteProbe, EveryAuthoredAnchorIsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    struct Row { uint32 entry; char const* name; };
    Row const rows[] = {
        { CertObj(1),   "A: entrance -> AT 5140"      },
        { NPC_SVALA,    "B: AT 5140 -> Svala"         },
        { CertObj(2),   "C: Svala -> Stasis Generator"},
        { NPC_PALEHOOF, "D: Stasis -> Palehoof"       },
        { CertObj(3),   "E: Palehoof -> AT 4991"      },
        { NPC_YMIRON,   "G: Skadi landing -> Ymiron"  },
    };

    std::printf("\n=== Utgarde Pinnacle (575) authored-anchor snap ===\n");
    for (Row const& row : rows)
    {
        std::vector<WaypointHint> const* route = CertRoute(row.entry);
        ASSERT_NE(route, nullptr) << "no authored route for leg " << row.name;
        ASSERT_FALSE(route->empty());

        std::printf("  --- %s (%zu anchors) ---\n", row.name, route->size());
        for (std::size_t i = 0; i < route->size(); ++i)
        {
            WaypointHint const& h = (*route)[i];

            G3D::Vector3 snapped;
            bool const ok = DcNavHarness::NearestPoint(mesh.get(), h.x, h.y, h.z,
                                                       CERT_SNAP_H, CERT_SNAP_V, snapped);
            float const delta =
                ok ? std::sqrt((snapped.x - h.x) * (snapped.x - h.x) +
                               (snapped.y - h.y) * (snapped.y - h.y) +
                               (snapped.z - h.z) * (snapped.z - h.z))
                   : -1.0f;

            std::printf("    [%2zu] (%8.2f, %9.2f, %7.2f) -> ", i, h.x, h.y, h.z);
            if (ok)
                std::printf("(%8.2f, %9.2f, %7.2f)  d=%.2f\n",
                            snapped.x, snapped.y, snapped.z, delta);
            else
                std::printf("OFF MESH\n");

            EXPECT_TRUE(ok) << row.name << " anchor " << i << " at (" << h.x << ", " << h.y
                            << ", " << h.z << ") is off the navmesh — the clear would walk "
                               "the party into geometry";
            EXPECT_GT(h.z, CERT_FLOOR_Z)
                << row.name << " anchor " << i << " is authored at z " << h.z
                << ", below the dungeon's own floor";
            if (ok)
            {
                EXPECT_LT(delta, CERT_SNAP_TOLERANCE)
                    << row.name << " anchor " << i << " snapped " << delta
                    << "yd — it is not standing where it was authored";
                EXPECT_GT(snapped.z, CERT_FLOOR_Z)
                    << row.name << " anchor " << i << " SNAPS to z " << snapped.z;
            }
        }
    }
    std::printf("===================================================\n");
}

// LEG A MUST ROUND THE NORTH-WEST BEND IN SHORT STEPS. This is a pinned
// regression on one corner, not a general law about anchor spacing, and the
// difference matters — see the survey at the bottom of this comment.
//
// The corner where the north corridor (x ~478-482) meets the west corridor
// (y ~-256) is solid rock: the whole quadrant x 466-474, y -259..-268 has no
// floor at any dungeon z. Leg A used to cross it as ONE 18.2yd chord, anchor 8
// straight to anchor 9, and in tr-20260902-101652-2 the tank wedged there for
// 11 minutes — 881 consecutive "Resnap failed, falling through", pinned at
// (478.6, -265.5, 104.7), 0 of 7 pulls advanced, run lost to no_progress.
//
// The mechanism is a length one, which is what makes it assertable:
//
//   * DC_REANCHOR_DISTANCE is 12yd. Arriving at an anchor whose successor is
//     farther than that makes TryReanchorStaleCursor fire every tick.
//   * The only way out is DungeonPathFollower::Resnap — forward-only, and gated
//     on BotCanWalk, a Detour raycast asking whether the straight leg to a
//     forward route point stays on the mesh.
//   * Around this corner nothing forward is straight-line walkable, so the
//     re-anchor could never clear.
//
// The fix rounds the bend on the corridor Detour actually walks (vertices 38-40
// of the polyline TheDesignedProgressionLegs prints, taken verbatim), which
// leaves every step across it UNDER the re-anchor distance. Then the corner never
// asks Resnap for anything, and its geometry stops mattering.
//
// WHY THIS IS NOT ASSERTED ROUTE-WIDE. The tempting generalisation — "no long
// chord may leave the navmesh" — is false on this very route, and was measured
// false rather than assumed:
//
//   * Leg A 6->7 is 20.0yd and its straight line clips the corridor's east wall
//     by ~0.35yd (no floor at (483.85, -300.4)). Walked every passing run.
//   * Leg C 7->8 has EVERY interior sample off-mesh and a 3.98yd plan-view drift
//     — geometrically worse than the chord that lost the run — and is walked
//     every passing run, because at 10.1yd it is under the re-anchor distance and
//     never asks Resnap anything.
//
// So a route-wide version would fail on legs that demonstrably work while still
// passing chords that do not. Until there is a rule that separates those cases,
// this stays a pinned corner. Do not "fix" Leg C 7->8 on geometry alone.
TEST(UtgardePinnacleRouteProbe, LegARoundsTheNorthWestBendInShortSteps)
{
    // Endpoints of the bend, as authored: the last anchor in the north corridor
    // and the first in the west one. Matched by position, not index, so inserting
    // anchors elsewhere on the leg does not silently retarget this test.
    constexpr float BEND_ENTRY_X = 478.99f, BEND_ENTRY_Y = -268.76f;
    constexpr float BEND_EXIT_X  = 465.51f, BEND_EXIT_Y  = -256.54f;
    constexpr float MATCH_TOLERANCE = 1.0f;

    // DcAdvanceAction's DC_REANCHOR_DISTANCE. A local literal on purpose: this
    // asserts a ROUTE property, so retuning that constant should mean re-deriving
    // this bend, not letting the assertion drift with it.
    constexpr float REANCHOR_DISTANCE = 12.0f;

    std::vector<WaypointHint> const* route = CertRoute(CertObj(1));
    ASSERT_NE(route, nullptr) << "no authored route for Leg A";

    auto findAnchor = [&](float x, float y) -> std::size_t
    {
        for (std::size_t i = 0; i < route->size(); ++i)
            if (std::fabs((*route)[i].x - x) < MATCH_TOLERANCE &&
                std::fabs((*route)[i].y - y) < MATCH_TOLERANCE)
                return i;
        return route->size();
    };

    std::size_t const entry = findAnchor(BEND_ENTRY_X, BEND_ENTRY_Y);
    std::size_t const exit  = findAnchor(BEND_EXIT_X, BEND_EXIT_Y);

    ASSERT_LT(entry, route->size())
        << "Leg A no longer has an anchor at the bend's north-corridor entry ("
        << BEND_ENTRY_X << ", " << BEND_ENTRY_Y << ") — re-derive this test from the "
           "corridor printed by TheDesignedProgressionLegs";
    ASSERT_LT(exit, route->size())
        << "Leg A no longer has an anchor at the bend's west-corridor exit ("
        << BEND_EXIT_X << ", " << BEND_EXIT_Y << ") — re-derive this test from the "
           "corridor printed by TheDesignedProgressionLegs";
    ASSERT_LT(entry, exit) << "the bend's anchors are out of order on Leg A";

    std::printf("\n=== Utgarde Pinnacle (575) Leg A north-west bend ===\n");
    for (std::size_t i = entry; i < exit; ++i)
    {
        WaypointHint const& a = (*route)[i];
        WaypointHint const& b = (*route)[i + 1];
        float const dx = b.x - a.x, dy = b.y - a.y;
        float const len = std::sqrt(dx * dx + dy * dy);

        std::printf("  %2zu -> %-2zu  (%7.2f,%8.2f) -> (%7.2f,%8.2f)  %5.2fyd\n",
                    i, i + 1, a.x, a.y, b.x, b.y, len);

        EXPECT_LE(len, REANCHOR_DISTANCE)
            << "Leg A anchors " << i << " and " << (i + 1) << " are " << len
            << "yd apart across the north-west bend. Past DC_REANCHOR_DISTANCE the tank "
               "re-anchors on arrival every tick, and around this corner Resnap's "
               "BotCanWalk gate can never clear — that is the permanent wedge from "
               "tr-20260902-101652-2. Round the bend on the corridor printed by "
               "TheDesignedProgressionLegs, taking its vertices verbatim rather than "
               "shortening it.";
    }
    std::printf("===================================================\n");
}

// Each leg must be ONE CONTINUOUS CORRIDOR from its first anchor to its last,
// with no ledge in it and a length that has not moved. Two certified halves that
// do not JOIN are still a party standing at a wall.
TEST(UtgardePinnacleRouteProbe, EveryLegRoutesEndToEndWithNoLedge)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    struct Row { uint32 entry; char const* name; float minLen; float maxLen; };
    Row const rows[] = {
        { CertObj(1),   "A: entrance -> AT 5140",       300.0f, 430.0f },
        { NPC_SVALA,    "B: AT 5140 -> Svala",           80.0f, 130.0f },
        { CertObj(2),   "C: Svala -> Stasis Generator", 190.0f, 290.0f },
        { NPC_PALEHOOF, "D: Stasis -> Palehoof",         70.0f, 100.0f },
        { CertObj(3),   "E: Palehoof -> AT 4991",        55.0f,  85.0f },
        { NPC_YMIRON,   "G: Skadi landing -> Ymiron",   300.0f, 440.0f },
    };

    std::printf("\n=== Utgarde Pinnacle (575) leg certification ===\n");
    for (Row const& row : rows)
    {
        std::vector<WaypointHint> const* route = CertRoute(row.entry);
        ASSERT_NE(route, nullptr);
        ASSERT_GE(route->size(), 2u);

        WaypointHint const& a = route->front();
        WaypointHint const& b = route->back();
        DcNavHarness::RouteResult const r = DcNavHarness::Route(
            mesh.get(), MAP_UTGARDE_PINNACLE, a.x, a.y, a.z, b.x, b.y, b.z);

        std::printf("  %-30s reachable=%d complete=%d len2d=%7.1f maxStepZ=%.2f %s\n",
                    row.name, r.reachable, r.corridorComplete, r.routeLength2d,
                    r.maxStepZ, r.failureReason.c_str());

        EXPECT_TRUE(r.reachable)
            << row.name << ": the first anchor cannot path to the last one at all";
        EXPECT_TRUE(r.corridorComplete)
            << row.name << ": the corridor stops short of the leg's own destination";
        EXPECT_LT(r.maxStepZ, CERT_MAX_STEP_Z)
            << row.name << ": the corridor contains a " << r.maxStepZ
            << "yd vertical step — that is a ledge, and none of these legs has one";
        EXPECT_GT(r.routeLength2d, row.minLen)
            << row.name << " has been rerouted (" << r.routeLength2d << "yd)";
        EXPECT_LT(r.routeLength2d, row.maxLen)
            << row.name << " has been rerouted (" << r.routeLength2d
            << "yd) — re-author its anchors from the polyline printed above";

        for (auto const& p : r.points)
            EXPECT_GT(p.z, CERT_FLOOR_Z)
                << row.name << ": the corridor drops to z " << p.z << ", below the dungeon";
    }
    std::printf("================================================\n");
}

// THE DRY-GROUND ASSERTION, and on this map it is expected to be a clean
// negative — which is exactly why it is worth writing down.
//
// Map 575 has no water on any designed leg, so every anchor should read dry. The
// leg to watch is C, whose switchback drops to z 75-81 through the lower ring: a
// wet anchor would hide there, and the Azjol-Nerub lake
// ([[dc-an-lower-kingdom-is-flooded]]) is what happens when nobody asks. A route
// can sit perfectly on the navmesh and still have the party wading its length,
// because NAV_WATER and NAV_SLIME polys ARE navigable mesh at the liquid surface.
//
// NO CONTROL POINT, unlike the Halls of Lightning version of this test, and that
// is the honest difference: there is no known liquid surface on map 575 to prove
// the probe is measuring anything. So this test can only fail loudly and cannot
// pass meaningfully — treat a green here as "no anchor moved onto liquid", never
// as "the liquid test works".
TEST(UtgardePinnacleRouteProbe, EveryAuthoredAnchorStandsOnDryGround)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    for (uint32 entry : { CertObj(1), NPC_SVALA, CertObj(2), NPC_PALEHOOF,
                          CertObj(3), NPC_YMIRON })
    {
        std::vector<WaypointHint> const* route = CertRoute(entry);
        ASSERT_NE(route, nullptr);
        for (std::size_t i = 0; i < route->size(); ++i)
        {
            WaypointHint const& h = (*route)[i];
            float const drop = CertGroundDrop(mesh.get(), h.x, h.y, h.z);
            EXPECT_LT(drop, CERT_DRY_TOLERANCE)
                << "route " << entry << " anchor " << i << " at (" << h.x << ", " << h.y
                << ", " << h.z << ") stands on liquid — the party wades this leg";
        }
    }
}

// THE DOOR PROXIMITY GATE, measured against the real corridor rather than
// against the decimated anchors.
//
// The anchors are 16-20yd apart, so an anchor-only measurement can pass while the
// corridor BETWEEN two anchors threads a doorway. This routes each pre-Skadi leg
// for real and measures every corridor point.
TEST(UtgardePinnacleRouteProbe, NoPreSkadiCorridorThreadsAPortcullis)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    // Everything up to and including the gauntlet entry. The post-Skadi leg to
    // Ymiron is excluded on purpose: it passes 192173 at 3.1yd, and by the time it
    // is walked SetData(DATA_SKADI, DONE) has opened that gate permanently.
    struct Row { uint32 entry; char const* name; };
    Row const rows[] = {
        { CertObj(1),   "A: entrance -> AT 5140"       },
        { NPC_SVALA,    "B: AT 5140 -> Svala"          },
        { CertObj(2),   "C: Svala -> Stasis Generator" },
        { NPC_PALEHOOF, "D: Stasis -> Palehoof"        },
        { CertObj(3),   "E: Palehoof -> AT 4991"       },
    };

    // A door origin sits below its own threshold (both of these snap +3.92 and
    // +1.83), so the proximity is measured in 2D — the question is whether the
    // corridor runs through the DOORWAY, and its floor is the party's floor.
    std::printf("\n=== Utgarde Pinnacle (575) pre-Skadi door clearance ===\n");
    for (Row const& row : rows)
    {
        std::vector<WaypointHint> const* route = CertRoute(row.entry);
        ASSERT_NE(route, nullptr);
        ASSERT_GE(route->size(), 2u);

        DcNavHarness::RouteResult const r = DcNavHarness::Route(
            mesh.get(), MAP_UTGARDE_PINNACLE,
            route->front().x, route->front().y, route->front().z,
            route->back().x, route->back().y, route->back().z);
        ASSERT_FALSE(r.points.empty()) << row.name << " produced no corridor";

        float nearestSkadi = 1e9f;
        float nearestYmiron = 1e9f;
        for (auto const& p : r.points)
        {
            nearestSkadi = std::min(
                nearestSkadi, std::hypot(p.x - DOOR_SKADI.x, p.y - DOOR_SKADI.y));
            nearestYmiron = std::min(
                nearestYmiron, std::hypot(p.x - DOOR_ENTRY.x, p.y - DOOR_ENTRY.y));
        }

        std::printf("  %-30s 192173 %7.2fyd   192174 %7.2fyd\n",
                    row.name, nearestSkadi, nearestYmiron);

        // 25yd is the band a phantom door prop is auto-paused within
        // ([[dc-scriptonly-is-not-navigation-ignored]]). A corridor inside it is a
        // run that pauses on a gate it was never meant to reach — which is exactly
        // how tr-20260902-083808-1 spent its whole 314 seconds.
        EXPECT_GT(nearestSkadi, 25.0f)
            << row.name << " passes " << nearestSkadi
            << "yd from GO 192173, which does not open until Skadi dies";
        EXPECT_GT(nearestYmiron, 25.0f)
            << row.name << " passes " << nearestYmiron
            << "yd from GO 192174, which is the LAST boss's exit and never opens "
               "during the run";
    }
    std::printf("=======================================================\n");
}

// THE HARPOON LEG (F), which is the one designed leg with no anchor row: the
// driver walks it on a single long-haul spline, so what has to be true is simply
// that the corridor exists, is walkable, and ends at the launcher platform.
//
// It is also the leg with the tightest positional constraint in the dungeon: it
// runs at y ~ -509 down the middle of the Flame Breath Trigger carpet, and the
// harpoon pocket at its far end has to stay inside 40yd of the nearest trigger or
// the gauntlet's own 6-second reset check counts zero and evades Skadi.
TEST(UtgardePinnacleRouteProbe, TheHarpoonLegIsWalkableAndEndsAtThePocket)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = Load(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const r = DcNavHarness::Route(
        mesh.get(), MAP_UTGARDE_PINNACLE,
        AT_SKADI_X, AT_SKADI_Y, AT_SKADI_Z,
        POCKET_X, POCKET_Y, POCKET_Z);

    std::printf("\n=== Utgarde Pinnacle (575) leg F: AT 4991 -> the harpoon pocket ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                r.maxStepZ, r.failureReason.c_str());
    for (std::size_t i = 0; i < r.points.size(); ++i)
        std::printf("  [pt %3zu] %9.2f, %9.2f, %7.2f\n",
                    i, r.points[i].x, r.points[i].y, r.points[i].z);
    std::printf("==================================================================\n");

    EXPECT_TRUE(r.reachable) << "the gauntlet trigger cannot path to the harpoon pocket";
    EXPECT_TRUE(r.corridorComplete) << "the corridor stops short of the launcher platform";
    EXPECT_LT(r.maxStepZ, CERT_MAX_STEP_Z);
    EXPECT_GT(r.routeLength2d, 140.0f);
    EXPECT_LT(r.routeLength2d, 200.0f)
        << "leg F has been rerouted (" << r.routeLength2d
        << "yd) — the driver walks it on one spline and the hall is supposed to be "
           "straight";

    // The pocket itself has to be standable, or the driver parks the party in a
    // wall for the whole of phase 1.
    G3D::Vector3 snapped;
    ASSERT_TRUE(DcNavHarness::NearestPoint(mesh.get(), POCKET_X, POCKET_Y, POCKET_Z,
                                           CERT_SNAP_H, CERT_SNAP_V, snapped))
        << "the harpoon pocket is off the navmesh";
    EXPECT_LT(std::hypot(snapped.x - POCKET_X, snapped.y - POCKET_Y), CERT_SNAP_TOLERANCE);
}
