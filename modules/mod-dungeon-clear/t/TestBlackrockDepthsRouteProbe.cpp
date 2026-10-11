/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Navmesh probe for the Blackrock Depths wing split (map 230): does an Upper
// City run, which starts at the portal and never clears the Detention Block,
// route clear of the Ring of Law?
//
// The Ring of Law is a sealed arena run by area trigger 1526: a player crossing
// the centre starts the gauntlet and slams the gates. A full-dungeon run went
// through it on purpose (it is a Detention Block encounter). An Upper City run
// must not stumble into it on the way to the Shadowforge Lock or beyond — if the
// navmesh ever routes a leg across the arena floor, the Upper City needs a
// wing-tagged DcNavPenaltyRegistry fence there, as Blackrock Spire has.
//
// The suite also PRINTS each leg's closest approach to every Detention Block
// boss, so a route that walks the party past one of them (a boss pull the run
// does not count) is visible.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='BlackrockDepthsRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    constexpr uint32_t BRD = 230;

    struct Pt { float x, y, z; char const* name; };

    // Area trigger 1526, the arena centre on the floor (BlackrockDepthsEvents).
    constexpr Pt ARENA = { 596.432f, -188.498f, -53.9f, "Ring of Law" };
    // The arena floor is ~40yd across; anything within this of the centre, on
    // the floor's level, is inside the sealed ring.
    constexpr float ARENA_FLOOR_R = 25.0f;
    constexpr float ARENA_FLOOR_DZ = 8.0f;

    // Portal, then the Upper City in its clear order (Shadowforge Lock first).
    std::vector<Pt> const UPPER_CITY = {
        { 456.93f,   34.09f, -68.09f, "portal" },
        { 615.61f,  -49.78f, -59.82f, "Shadowforge Lock" },
        { 652.4f,    21.4f,  -60.0f,  "Angerforge" },
        { 846.8f,    16.3f,  -53.6f,  "Argelmach" },
        { 878.1f,  -153.1f,  -49.8f,  "Hurley" },
        { 869.0f,  -225.0f,  -43.7f,  "Phalanx" },
        { 878.5f,  -167.7f,  -49.7f,  "Ribbly" },
        { 888.5f,  -177.9f,  -43.0f,  "Plugger" },
        { 1009.8f, -239.0f,  -61.3f,  "Flamelash" },
        { 1215.1f, -220.7f,  -85.6f,  "The Seven" },
        { 1380.7f, -659.3f,  -92.0f,  "Magmus" },
        { 1380.2f, -831.6f,  -87.6f,  "Thaurissan" },
    };

    std::vector<Pt> const DETENTION_BLOCK = {
        { 310.6f, -146.3f, -70.3f, "Gerstahn" },
        { 615.5f, -267.4f, -83.6f, "Roccor" },
        { 607.7f, -174.8f, -84.5f, "Grebmar" },
        { 530.2f, -243.9f, -43.0f, "Loregrain" },
        { 893.5f, -267.1f, -71.9f, "Incendius" },
        { 823.4f, -342.3f, -50.1f, "Stilgiss" },
        { 963.3f, -343.7f, -71.7f, "Darkvire" },
        { 702.4f,  184.5f, -72.0f, "Bael'Gar" },
    };

    std::shared_ptr<dtNavMesh> LoadOrSkip()
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
            return nullptr;
        return DcNavHarness::LoadMap(dir, BRD);
    }

    Pt SnapOrKeep(dtNavMesh const* mesh, Pt p)
    {
        G3D::Vector3 s;
        if (DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, 10.0f, 20.0f, s))
            return { s.x, s.y, s.z, p.name };
        return p;
    }

    float Dist3(G3D::Vector3 const& a, Pt const& b)
    {
        float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The polyline sampled every 2yd: its corners alone can straddle the arena.
    std::vector<G3D::Vector3> Densify(std::vector<G3D::Vector3> const& pts)
    {
        std::vector<G3D::Vector3> out;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        {
            G3D::Vector3 const d = pts[i + 1] - pts[i];
            int const steps = std::max(1, static_cast<int>(d.length() / 2.0f));
            for (int k = 0; k < steps; ++k)
                out.push_back(pts[i] + d * (static_cast<float>(k) / steps));
        }
        if (!pts.empty())
            out.push_back(pts.back());
        return out;
    }

    bool OnArenaFloor(G3D::Vector3 const& p)
    {
        float const dx = p.x - ARENA.x, dy = p.y - ARENA.y;
        return std::sqrt(dx * dx + dy * dy) < ARENA_FLOOR_R && std::fabs(p.z - ARENA.z) < ARENA_FLOOR_DZ;
    }
}

TEST(BlackrockDepthsRouteProbe, UpperCityRouteStaysOutOfTheRingOfLaw)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 230 mmaps missing";

    std::printf("Blackrock Depths — Upper City legs (closest approach to the arena / Detention Block bosses)\n");
    for (std::size_t i = 0; i + 1 < UPPER_CITY.size(); ++i)
    {
        Pt const from = SnapOrKeep(mesh.get(), UPPER_CITY[i]);
        Pt const to = SnapOrKeep(mesh.get(), UPPER_CITY[i + 1]);
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), BRD, from.x, from.y, from.z, to.x, to.y, to.z);
        std::printf("  %-16s -> %-16s reachable=%d complete=%d len2d=%.1f pts=%zu %s\n", from.name, to.name,
                    r.reachable, r.corridorComplete, r.routeLength2d, r.points.size(), r.failureReason.c_str());
        EXPECT_TRUE(r.reachable) << from.name << " -> " << to.name;

        std::vector<G3D::Vector3> const walk = Densify(r.points);
        float arenaMin = std::numeric_limits<float>::max();
        for (G3D::Vector3 const& p : walk)
        {
            arenaMin = std::min(arenaMin, Dist3(p, ARENA));
            EXPECT_FALSE(OnArenaFloor(p)) << from.name << " -> " << to.name << " crosses the Ring of Law at ("
                                          << p.x << ", " << p.y << ", " << p.z << ")";
        }
        std::printf("      arena %.1fyd", arenaMin);
        for (Pt const& boss : DETENTION_BLOCK)
        {
            float d = std::numeric_limits<float>::max();
            for (G3D::Vector3 const& p : walk)
                d = std::min(d, Dist3(p, boss));
            if (d < 60.0f)
                std::printf(" | %s %.1fyd", boss.name, d);
        }
        std::printf("\n");
    }
}
