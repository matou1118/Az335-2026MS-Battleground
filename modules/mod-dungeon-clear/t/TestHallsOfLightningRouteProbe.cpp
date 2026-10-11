/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Halls of Lightning (map 602) route
// through the Slag Furnace, General Bjarngrim -> Volkhan.
//
// WHY THIS IS A GATE AND NOT A NICETY. Map 602's mmtiles carry flat sheets far
// below the dungeon — a continuous surface at z ~ -1.9 and another at z ~ -13.9
// under essentially the whole footprint — so it is in the flat-grid-height family
// ([[ac-map601-flat-gridheight-zero]]) and an anchor written from a script
// literal rather than from a probed surface resolves tens of yards underground.
// The open centre of Bjarngrim's ring has NO dungeon floor at all: the column at
// (1330, 30) returns 143.02, -1.88, -13.88 and nothing else.
//
// The suite also PRINTS the routed polylines for all four legs. That is
// deliberate: this is the tool the anchors were authored with (route the leg,
// decimate the polyline — [[dc-navharness-prints-the-route]]), so an mmaps regen
// that moves the corridor is re-authored the same way instead of by hand.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, same contract as the Azjol-Nerub,
// Blackwing Lair, Mechanar and Ramparts probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='HallsOfLightningRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace DcHallsOfLightning;

    // The five fixed points the four legs run between. Boss coordinates are the
    // live `creature` rows; the entrance is DcTestDungeonRegistry's map-602 row,
    // i.e. the areatrigger_teleport target a walked-in party lands on.
    struct Pt { float x, y, z; char const* name; };

    constexpr Pt ENTRANCE  = { 1331.47f,  259.62f, 53.40f, "entrance"  };
    constexpr Pt BJARNGRIM = { 1262.00f,  -26.90f, 33.50f, "Bjarngrim" };
    constexpr Pt VOLKHAN   = { 1332.38f, -102.08f, 56.80f, "Volkhan"   };
    constexpr Pt IONAR     = { 1081.99f, -261.81f, 61.29f, "Ionar"     };
    constexpr Pt LOKEN     = { 1186.47f,   33.83f, 60.81f, "Loken"     };

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this anchor is not standing anywhere near here", not "something was found
    // across the pit"; vertical covers the ordinary float between an authored z
    // and the mesh surface under it without reaching either of the under-map
    // sheets, the nearest of which is 25yd below the pit floor.
    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 4.0f;

    // How far the mesh may move an authored anchor before it stops being an
    // authored anchor. A couple of yards is ordinary detail-mesh float; more than
    // that and the point was written somewhere the party cannot stand.
    constexpr float SNAP_TOLERANCE = 2.5f;

    // THE TRAPDOOR TEST. Nothing on the walked route belongs below this: the pit
    // floor, the lowest ground the party ever stands on, probes at z 23.88, and
    // the flanking slime surfaces at 20.18. Anything under 15 is one of the two
    // under-map sheets.
    constexpr float TRAPDOOR_Z = 15.0f;

    // The crossing's routed 2D length, pinned in a band. The measured
    // Bjarngrim -> Volkhan corridor is 395.4yd; a regen that reroutes the leg
    // moves this by tens of yards and must trip red rather than silently changing
    // what the transit crosses.
    constexpr float LEG_B_MIN_2D = 330.0f;
    constexpr float LEG_B_MAX_2D = 470.0f;

    // Largest vertical step between consecutive corridor points that is still a
    // WALK. The measured leg is 2.54. Anything above this is a ledge the party
    // would have to fall down, and this route has none.
    constexpr float MAX_STEP_Z = 6.0f;

    // How far the NAV_GROUND-only snap may drop below the permissive one before
    // the point counts as standing on liquid. The slime sits ~3.7yd under the
    // walkway, so anything over a couple of yards is unambiguous.
    constexpr float DRY_TOLERANCE = 2.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 602";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-602 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    std::vector<WaypointHint> const* Route()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_VOLKHAN);
    }

    // Route a leg and print its polyline. The printing IS the authoring tool.
    DcNavHarness::RouteResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("\n=== Halls of Lightning (602): %s -> %s ===\n", a.name, b.name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                    r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                    r.maxStepZ, r.failureReason.c_str());
        for (std::size_t i = 0; i < r.points.size(); ++i)
            std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z);
        return r;
    }

    // Vertical drop from the permissive snap to the NAV_GROUND-only snap, or a
    // large sentinel when no ground poly is in range at all. Both mean liquid.
    float GroundDrop(dtNavMesh const* mesh, float x, float y, float z)
    {
        G3D::Vector3 any;
        if (!DcNavHarness::NearestPoint(mesh, x, y, z, SNAP_H, 15.0f, any))
            return 1e9f;
        G3D::Vector3 ground;
        if (!DcNavHarness::NearestPoint(mesh, x, y, z, SNAP_H, 15.0f, ground, NAV_GROUND))
            return 1e9f;
        return std::fabs(any.z - ground.z);
    }
}

// Every authored anchor must be somewhere a bot can actually stand — and, on this
// map, somewhere ABOVE the trapdoor.
TEST(HallsOfLightningRouteProbe, EveryAnchorIsOnTheMeshAndAboveTheTrapdoor)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr) << "no authored Bjarngrim -> Volkhan route on map 602";
    ASSERT_GT(route->size(), TRANSIT_END_ANCHOR_INDEX);

    std::printf("=== Halls of Lightning (602) route — anchor snap ===\n");
    for (std::size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        char const* what = (i == TRANSIT_STAGE_ANCHOR_INDEX) ? "staging"
                         : (i == TRANSIT_END_ANCHOR_INDEX)   ? "mid ledge"
                         : (i == 0)                          ? "Bjarngrim"
                         : (i + 1 == route->size())          ? "Volkhan"
                         : (i > TRANSIT_STAGE_ANCHOR_INDEX && i < TRANSIT_END_ANCHOR_INDEX)
                               ? "crossing"
                               : "anchor";

        G3D::Vector3 snapped;
        bool const ok =
            DcNavHarness::NearestPoint(mesh.get(), h.x, h.y, h.z, SNAP_H, SNAP_V, snapped);
        float const delta =
            ok ? std::sqrt((snapped.x - h.x) * (snapped.x - h.x) +
                           (snapped.y - h.y) * (snapped.y - h.y) +
                           (snapped.z - h.z) * (snapped.z - h.z))
               : -1.0f;

        std::printf("  [%-9s %2zu] (%8.2f, %9.2f, %6.2f)  ->  ", what, i, h.x, h.y, h.z);
        if (ok)
            std::printf("(%8.2f, %9.2f, %6.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, delta);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << what << " " << i << " at (" << h.x << ", " << h.y << ", " << h.z
                        << ") is off the navmesh — the transit would walk the party into geometry";
        EXPECT_GT(h.z, TRAPDOOR_Z)
            << "anchor " << i << " is authored at z " << h.z
            << ", below the dungeon — that is one of map 602's under-map sheets";
        if (ok)
        {
            EXPECT_LT(delta, SNAP_TOLERANCE)
                << what << " " << i << " snapped " << delta
                << "yd — it is not standing where it was authored";
            EXPECT_GT(snapped.z, TRAPDOOR_Z)
                << "anchor " << i << " SNAPS to z " << snapped.z << " — the trapdoor";
        }
    }
    std::printf("====================================================\n");
}

// The crossing has to be one continuous corridor from the staging point to the
// mid ledge, with no ledge in it and a length that has not moved.
TEST(HallsOfLightningRouteProbe, TheStagingPointRoutesToTheMidLedge)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const r =
        DcNavHarness::Route(mesh.get(), MAP_ID,
                            TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z,
                            TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z);

    std::printf("=== Halls of Lightning (602) slag furnace corridor ===\n");
    std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                r.maxStepZ, r.failureReason.c_str());
    for (std::size_t i = 0; i < r.points.size(); ++i)
        std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                    i, r.points[i].x, r.points[i].y, r.points[i].z);
    std::printf("======================================================\n");

    EXPECT_TRUE(r.reachable) << "the staging point cannot path to the mid ledge at all";
    EXPECT_TRUE(r.corridorComplete)
        << "the corridor stops short of the mid ledge — the crossing would end in the pit";
    EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
        << "the corridor contains a " << r.maxStepZ << "yd vertical step — that is a ledge";
    for (auto const& p : r.points)
        EXPECT_GT(p.z, TRAPDOOR_Z) << "the corridor drops to z " << p.z << " — the trapdoor";
}

// THE WHOLE LEG, end to end, plus the other three. The row has to be one
// continuous corridor from Bjarngrim to Volkhan — two certified halves that do
// not JOIN are still a party standing at a wall — and the three legs DC does not
// author still have to be walkable, because nothing else checks them.
TEST(HallsOfLightningRouteProbe, TheFourLegsAreOneContinuousDungeon)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const a = PrintLeg(mesh.get(), ENTRANCE, BJARNGRIM);
    DcNavHarness::RouteResult const b = PrintLeg(mesh.get(), BJARNGRIM, VOLKHAN);
    DcNavHarness::RouteResult const c = PrintLeg(mesh.get(), VOLKHAN, IONAR);
    DcNavHarness::RouteResult const d = PrintLeg(mesh.get(), IONAR, LOKEN);
    std::printf("\n");

    for (auto const* leg : { &a, &b, &c, &d })
    {
        EXPECT_TRUE(leg->reachable);
        EXPECT_TRUE(leg->corridorComplete);
        EXPECT_LT(leg->maxStepZ, MAX_STEP_Z);
    }

    // Only leg B is authored, so only leg B's length is pinned.
    EXPECT_GT(b.routeLength2d, LEG_B_MIN_2D);
    EXPECT_LT(b.routeLength2d, LEG_B_MAX_2D)
        << "the Bjarngrim -> Volkhan corridor has been rerouted (" << b.routeLength2d
        << "yd) — re-author the anchors from the polyline printed above";
}

// The authored anchors must be DRY. The pit's flanking moats are NAV_SLIME polys
// (flags 0x04) — navigable mesh at the liquid surface, 3.7yd under the walkway —
// so a route can sit perfectly on the navmesh and still have the party wading the
// length of the gauntlet, which is exactly the Azjol-Nerub lake defect
// ([[dc-an-lower-kingdom-is-flooded]]).
TEST(HallsOfLightningRouteProbe, EveryCrossingAnchorStandsOnDryGround)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);

    // CONTROL FIRST: the middle of the west moat MUST read wet, or the probe is
    // not measuring liquid and every assertion below is worthless.
    float const moat = GroundDrop(mesh.get(), 1284.0f, -140.0f, 20.18f);
    std::printf("  [control] west moat (1284.00, -140.00, 20.18)  ground-drop %.2f\n", moat);
    ASSERT_GT(moat, DRY_TOLERANCE)
        << "the west slag moat reads as dry ground — either the mesh changed or this "
           "probe is no longer measuring liquid";

    for (std::size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        float const drop = GroundDrop(mesh.get(), h.x, h.y, h.z);
        std::printf("  [anchor %2zu] (%8.2f, %9.2f, %6.2f)  ground-drop %.2f\n",
                    i, h.x, h.y, h.z, drop);
        EXPECT_LT(drop, DRY_TOLERANCE)
            << "anchor " << i << " at (" << h.x << ", " << h.y << ", " << h.z
            << ") stands on NAV_SLIME — the party wades this leg instead of walking it";
    }
}

// THE PENALTY ROWS, against the real mesh rather than against eyeballed wall
// coordinates — which is what [[dc-nav-fence-authoring]] asks for and the one
// check that catches a row that has become a cage. The walkway between the two
// moats is only ~41yd wide at its narrowest, and an over-sized row would pinch it.
TEST(HallsOfLightningRouteProbe, TheSlimeRowsCoverTheMoatsAndNoneOfTheWalkway)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("=== Halls of Lightning (602) slime rows vs the real mesh ===\n");
    // The map the rows were authored from. 'S' slime, '#' dry, '.' no mesh,
    // lower-case where the registry taxes the column.
    for (float y = -204.0f; y <= -120.0f; y += 4.0f)
    {
        std::printf("  y=%7.1f  ", y);
        for (float x = 1274.0f; x <= 1386.0f; x += 4.0f)
        {
            G3D::Vector3 a, sl;
            if (!DcNavHarness::NearestPoint(mesh.get(), x, y, 22.0f, 2.0f, 6.0f, a))
            {
                std::printf(".");
                continue;
            }
            bool const w = DcNavHarness::NearestPoint(mesh.get(), x, y, a.z, 2.0f, 1.0f, sl,
                                                      NAV_SLIME) &&
                           std::fabs(sl.z - a.z) < 1.0f;
            bool const t = DcNavPenaltyRegistry::IsInsideRegion(MAP_ID, x, y, a.z);
            std::printf("%c", w ? (t ? 's' : 'S') : (t ? 'x' : '#'));
        }
        std::printf("\n");
    }
    std::printf("  x 1274..1386 step 4 | S slime untaxed, s slime taxed, # dry, x DRY TAXED\n");

    // The band the party actually walks. The dry span narrows to x 1314..1346
    // (y -124, -140, -144, -188), the authored route hugs x 1330..1341, and a
    // penalty row that reached into this is a row that has pinched the only way
    // through the pit.
    constexpr float WALKWAY_MIN_X = 1314.0f;
    constexpr float WALKWAY_MAX_X = 1346.0f;

    // The narrowest dry span the pit may be left with after the rows are applied,
    // in 4yd grid columns. Nine is what the mesh itself offers at the pinch
    // points, so anything less means a row has eaten into it.
    constexpr int MIN_FREE_COLUMNS = 9;

    int taxedWalkway = 0;
    int taxedEdge = 0;
    int untaxedSlime = 0;
    int dryCells = 0;
    int slimeCells = 0;
    int worstFreeRun = 1000;
    float worstFreeRunY = 0.0f;

    // The same 4yd grid the rows were authored from.
    for (float y = -204.0f; y <= -120.0f; y += 4.0f)
    {
        int freeRun = 0;
        int bestRun = 0;
        for (float x = 1274.0f; x <= 1386.0f; x += 4.0f)
        {
            G3D::Vector3 any;
            if (!DcNavHarness::NearestPoint(mesh.get(), x, y, 22.0f, 2.0f, 6.0f, any))
            {
                freeRun = 0;
                continue;
            }

            // ASK THE POLY DIRECTLY. Two indirect tests were tried first and both
            // were wrong in an instructive way: "did the NAV_GROUND snap find
            // anything in the box" calls a shoreline column dry (it carries BOTH
            // a slime poly and the walkway shelf 3.7yd above), and the
            // Azjol-Nerub ground-drop test disagrees with itself at the channel
            // mouths, where the floor really is walkable at the slime's own
            // height. The question these rows are authored against is not "is
            // there ground nearby" but "is the surface a bot stands on here a
            // NAV_SLIME poly", and a NAV_SLIME-only snap that lands at the same
            // height as the permissive one answers exactly that.
            G3D::Vector3 slime;
            bool const wet =
                DcNavHarness::NearestPoint(mesh.get(), x, y, any.z, 2.0f, 1.0f, slime,
                                           NAV_SLIME) &&
                std::fabs(slime.z - any.z) < 1.0f;
            bool const taxed = DcNavPenaltyRegistry::IsInsideRegion(MAP_ID, x, y, any.z);

            if (wet)
            {
                ++slimeCells;
                if (!taxed)
                    ++untaxedSlime;
                freeRun = 0;
                continue;
            }

            ++dryCells;
            if (taxed)
            {
                freeRun = 0;
                if (x >= WALKWAY_MIN_X && x <= WALKWAY_MAX_X)
                {
                    ++taxedWalkway;
                    std::printf("  [TAXED WALKWAY] (%7.1f, %7.1f, %6.2f)\n", x, y, any.z);
                }
                else
                    ++taxedEdge;
                continue;
            }

            ++freeRun;
            if (freeRun > bestRun)
                bestRun = freeRun;
        }

        // A row with no dry ground at all is not part of the pit floor.
        if (bestRun > 0 && bestRun < worstFreeRun)
        {
            worstFreeRun = bestRun;
            worstFreeRunY = y;
        }
    }

    std::printf("  %d dry columns (%d taxed on the walkway, %d at the channel edges), "
                "%d slime columns (%d untaxed)\n",
                dryCells, taxedWalkway, taxedEdge, slimeCells, untaxedSlime);
    std::printf("  narrowest untaxed dry run: %d columns at y %.1f\n",
                worstFreeRun, worstFreeRunY);
    std::printf("============================================================\n");

    ASSERT_GT(slimeCells, 0) << "no slime found in the pit — the probe is measuring nothing";

    // THE PROPERTY THAT MATTERS. A row that reaches the band the route walks has
    // stopped being a fence and become a cage.
    EXPECT_EQ(taxedWalkway, 0)
        << taxedWalkway << " columns of the Slag Furnace walkway (x " << WALKWAY_MIN_X
        << ".." << WALKWAY_MAX_X << ") are inside a penalty region";

    // ...and the pit must still be crossable as one open corridor, row by row.
    EXPECT_GE(worstFreeRun, MIN_FREE_COLUMNS)
        << "the widest untaxed dry run at y " << worstFreeRunY << " is only " << worstFreeRun
        << " columns — the rows have pinched the walkway";

    // The rows are a COST, so a few columns at the ragged channel edges falling on
    // the wrong side of a straight polygon edge are tolerable in both directions;
    // a third of the liquid escaping them is not.
    EXPECT_LT(untaxedSlime, slimeCells / 3)
        << untaxedSlime << " of " << slimeCells
        << " slime columns are untaxed — the rows no longer describe the moats";
}
