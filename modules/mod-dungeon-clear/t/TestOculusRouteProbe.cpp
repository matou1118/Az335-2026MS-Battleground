/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for The Oculus (map 578) site table.
//
// The whole design rests on one claim about the navmesh: the rings are ISLANDS,
// and nothing walks between them — the portal is the only way from the entrance
// floor to Drakos's ring, and a drake is the only way anywhere above it. This
// probe checks the claim both ways: every pad, every landing lane and every clear
// target routes on its OWN island, and no route joins two islands. If one ever
// does, the site table is wrong (and a flight leg may be flying over a bridge).
//
// It PRINTS the routed polylines and the snaps, deliberately: this is the tool the
// site table was checked with.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS and
// GTEST_SKIPs when unset, the same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='OculusRouteProbe.*'
//
// It does NOT check the flight chords: the harness has no VMAP, and the chords are
// LOS-checked live when each leg is planned (a blocked one logs `DcOc WARN leg ...`).

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusFlightDecision.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace DcOculus;

    struct Pt { float x, y, z; char const* name; };

    constexpr Pt ENTRANCE = { ENTRANCE_X, ENTRANCE_Y, ENTRANCE_Z, "entrance" };
    constexpr Pt PORTAL = { PORTAL_X, PORTAL_Y, PORTAL_Z, "the Nexus Portal" };
    constexpr Pt LANDING = { PORTAL_LAND_X, PORTAL_LAND_Y, PORTAL_LAND_Z, "the portal landing" };
    constexpr Pt DRAKOS = { 947.79f, 1045.80f, 360.05f, "Drakos" };
    constexpr Pt VAROS = { 1285.60f, 1070.36f, 439.52f, "Varos" };

    // Where the givers stand once Drakos is dead (oculus.h *POS).
    constexpr Pt GIVERS[3] = {
        { 949.056f, 1032.97f, 359.967f, "Verdisa" },
        { 941.355f, 1044.26f, 359.967f, "Belgaristrasz" },
        { 943.202f, 1059.35f, 359.967f, "Eternos" },
    };

    // The ten Centrifuge Constructs (and the pads' Ring-Lords), from the `creature` table, by island.
    struct Island { uint8 site; std::vector<Pt> targets; };

    std::vector<Island> Islands()
    {
        return {
            { SITE_R2C, { { 1071.6f, 1103.8f, 433.0f, "construct (central, north arc)" },
                          { 1076.9f,  994.3f, 433.0f, "construct (central, south arc)" },
                          { 1124.4f, 1109.8f, 433.0f, "construct (central, north arc)" },
                          { 1129.7f,  993.2f, 433.0f, "construct (central, south arc)" } } },
            { SITE_R2S, { { 1023.2f,  893.3f, 439.5f, "construct (south)" },
                          { 1041.2f,  877.6f, 439.5f, "construct (south)" },
                          { 1045.2f,  899.4f, 439.5f, "construct (south)" },
                          { 1036.5f,  890.5f, 439.5f, "Ring-Lord Conjurer (south)" } } },
            { SITE_R2N, { { 1004.9f, 1203.0f, 439.5f, "construct (north)" },
                          { 1023.7f, 1190.9f, 439.5f, "construct (north)" },
                          { 1025.5f, 1210.9f, 439.5f, "construct (north)" },
                          { 1017.1f, 1202.0f, 439.5f, "Ring-Lord Sorceress (north)" } } },
            { SITE_R2V, { VAROS } },
            { SITE_R3P0, { { UROM_CORDS[0].x, UROM_CORDS[0].y, UROM_CORDS[0].z, "Urom cords[0]" } } },
            { SITE_R3P1, { { UROM_CORDS[1].x, UROM_CORDS[1].y, UROM_CORDS[1].z, "Urom cords[1]" } } },
            { SITE_R3P2, { { UROM_CORDS[2].x, UROM_CORDS[2].y, UROM_CORDS[2].z, "Urom cords[2]" } } },
            { SITE_R3IN, { { UROM_CORDS[3].x, UROM_CORDS[3].y, UROM_CORDS[3].z, "Urom cords[3] (the fight)" } } },
        };
    }

    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 6.0f;
    constexpr float SNAP_TOLERANCE = 3.0f;

    Pt PadOf(uint8 site)
    {
        OcSite const& r = SITES[site];
        return { r.padX, r.padY, r.padZ, r.name };
    }

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 578";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-578 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    void SnapCheck(dtNavMesh const* mesh, Pt const& p)
    {
        G3D::Vector3 snapped;
        bool const ok = DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, SNAP_H, SNAP_V, snapped);
        float const d = ok ? std::sqrt((p.x - snapped.x) * (p.x - snapped.x) + (p.y - snapped.y) * (p.y - snapped.y) +
                                       (p.z - snapped.z) * (p.z - snapped.z))
                           : -1.0f;
        std::printf("  [%-34s] (%7.2f, %7.2f, %7.2f)  ->  ", p.name, p.x, p.y, p.z);
        if (ok)
            std::printf("(%7.2f, %7.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, d);
        else
            std::printf("OFF MESH\n");
        EXPECT_TRUE(ok) << p.name << " at (" << p.x << ", " << p.y << ", " << p.z << ") is off the navmesh";
        if (ok)
            EXPECT_LT(d, SNAP_TOLERANCE) << p.name << " snapped " << d << "yd from where it was authored";
    }

    DcNavHarness::RouteResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult r = DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("\n=== The Oculus (578): %s -> %s ===\n", a.name, b.name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n", r.reachable,
                    r.corridorComplete, r.pointCount, r.routeLength2d, r.maxStepZ, r.failureReason.c_str());
        return r;
    }

    void ExpectRoutes(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r = PrintLeg(mesh, a, b);
        EXPECT_TRUE(r.reachable && r.corridorComplete)
            << a.name << " -> " << b.name << " does not route on its island: " << r.failureReason;
    }

    void ExpectNoRoute(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r = PrintLeg(mesh, a, b);
        EXPECT_FALSE(r.reachable && r.corridorComplete)
            << a.name << " -> " << b.name << " WALKS — the two are not separate islands, and the site table "
                                              "(and the flight leg between them) is wrong";
    }
}

TEST(OculusRouteProbe, EveryPadLaneAndAnchorIsOnTheNavmesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== The Oculus (578): pads, landing lanes, anchors ===\n");
    for (Pt const& p : { ENTRANCE, PORTAL, LANDING, DRAKOS, VAROS })
        SnapCheck(mesh.get(), p);
    for (Pt const& g : GIVERS)
        SnapCheck(mesh.get(), g);

    for (uint8 site = 0; site < SITE_COUNT; ++site)
    {
        SnapCheck(mesh.get(), PadOf(site));
        for (uint32 lane = 1; lane < 5; ++lane)
        {
            DcOculusFlight::Vec const v = DcOculusFlight::PadLanePoint(site, lane);
            std::string const name = std::string(SITES[site].name) + " lane " + std::to_string(lane);
            SnapCheck(mesh.get(), { v.x, v.y, v.z, name.c_str() });
        }
    }
    Pt const arena = { UROM_CORDS[3].x, UROM_CORDS[3].y, UROM_CORDS[3].z, "Urom cords[3] (the fight)" };
    SnapCheck(mesh.get(), arena);
}

// Walkthrough steps 1-2: the entrance floor routes to the portal and NOT to Drakos.
TEST(OculusRouteProbe, ThePortalIsTheOnlyWayToDrakos)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    ExpectRoutes(mesh.get(), ENTRANCE, PORTAL);
    ExpectNoRoute(mesh.get(), ENTRANCE, DRAKOS);
    ExpectNoRoute(mesh.get(), PORTAL, LANDING);

    ExpectRoutes(mesh.get(), LANDING, DRAKOS);
    for (Pt const& g : GIVERS)
        ExpectRoutes(mesh.get(), LANDING, g);
    ExpectRoutes(mesh.get(), LANDING, PadOf(SITE_R1_GIVERS));
}

// Each pad reaches every target of its own clear on foot.
TEST(OculusRouteProbe, EachPadRoutesToItsIslandsTargets)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    for (Island const& isl : Islands())
        for (Pt const& t : isl.targets)
            ExpectRoutes(mesh.get(), PadOf(isl.site), t);
}

// The column a blocked climb uses: no floor anywhere up the central shaft between
// the entrance floor and above the Ring 4 floor (only the basement, far below).
TEST(OculusRouteProbe, TheCentralShaftIsOpenAtEveryRing)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== The Oculus (578): the central shaft at (%.1f, %.1f) ===\n", SHAFT_X, SHAFT_Y);
    for (float z = 340.0f; z <= 640.0f; z += 10.0f)
    {
        G3D::Vector3 snapped;
        bool const floor = DcNavHarness::NearestPoint(mesh.get(), SHAFT_X, SHAFT_Y, z, 3.0f, 5.0f, snapped);
        if (floor)
            std::printf("  z %.0f: FLOOR at (%.2f, %.2f, %.2f)\n", z, snapped.x, snapped.y, snapped.z);
        EXPECT_FALSE(floor) << "a floor in the shaft at z " << z << " — the column is not open";
    }
}

// The assumption every flight leg rests on: no island walks to another.
TEST(OculusRouteProbe, NoRouteJoinsTwoIslands)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<uint8> const flights = { SITE_R1_GIVERS, SITE_R2C, SITE_R2S, SITE_R2N, SITE_R2V,
                                         SITE_R3P0, SITE_R3P1, SITE_R3P2, SITE_R3IN, SITE_R4 };
    for (std::size_t i = 0; i < flights.size(); ++i)
        for (std::size_t j = i + 1; j < flights.size(); ++j)
            ExpectNoRoute(mesh.get(), PadOf(flights[i]), PadOf(flights[j]));
}

// The central ring is a horseshoe, and its sweep walks it from the landing on one
// end of the break to the far end: every stop is on the navmesh, every walk from
// one stop to the next goes the sweep's way round (no longer than 1.5x its arc),
// and across the break there is no short walk at all.
TEST(OculusRouteProbe, TheCentralRingSweepWalksRoundTheHorseshoe)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<Pt> stops = { PadOf(SITE_R2C) };
    for (OcRingStop const& s : R2C_SWEEP)
        stops.push_back({ s.x, s.y, s.z, s.name });

    std::printf("\n=== The Oculus (578): the central ring sweep ===\n");
    for (Pt const& p : stops)
        SnapCheck(mesh.get(), p);

    float const tau = 2.0f * DcOculusFlight::kPi;
    auto const bearing = [](Pt const& p) { return std::atan2(p.y - SHAFT_Y, p.x - SHAFT_X); };
    auto const radius = [](Pt const& p) { return std::hypot(p.x - SHAFT_X, p.y - SHAFT_Y); };
    for (std::size_t i = 0; i + 1 < stops.size(); ++i)
    {
        Pt const& a = stops[i];
        Pt const& b = stops[i + 1];
        DcNavHarness::RouteResult const r = PrintLeg(mesh.get(), a, b);
        EXPECT_TRUE(r.reachable && r.corridorComplete) << a.name << " -> " << b.name << ": " << r.failureReason;

        float fall = bearing(a) - bearing(b);
        while (fall < 0.0f)
            fall += tau;
        float const arc = 0.5f * (radius(a) + radius(b)) * fall;
        std::printf("  the sweep's arc=%.1f\n", arc);
        EXPECT_LT(r.routeLength2d, 1.5f * arc) << a.name << " -> " << b.name << " does not walk the sweep's way round";
    }

    // The break, due south of the shaft: a step across it is a walk round the ring.
    float const deg = DcOculusFlight::kPi / 180.0f;
    Pt const west = { SHAFT_X + 60.0f * std::cos(170.0f * deg), SHAFT_Y + 60.0f * std::sin(170.0f * deg), 433.0f,
                      "the break's west end" };
    Pt const east = { SHAFT_X + 60.0f * std::cos(200.0f * deg), SHAFT_Y + 60.0f * std::sin(200.0f * deg), 433.0f,
                      "the break's east end" };
    DcNavHarness::RouteResult const across = PrintLeg(mesh.get(), west, east);
    EXPECT_GT(across.routeLength2d, 200.0f) << "the central ring has no break any more — re-author R2C_SWEEP";
}
