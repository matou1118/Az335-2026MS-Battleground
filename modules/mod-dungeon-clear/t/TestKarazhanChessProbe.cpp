/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Navmesh probe for Karazhan's Gamesman's Hall (map 532) — the chess plan's C0.2:
// how the raid gets from the hall door onto the floor, where the sideline the
// controllers park on stands, whether the waiting balcony a released controller
// is teleported to reaches the floor, and the walk out through the exit door to
// Netherspace and Prince.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='KarazhanChessProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourStatus.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcChessBoard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <set>

using namespace DcKarazhan;

namespace
{
    std::shared_ptr<dtNavMesh> LoadOrSkip()
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
            return nullptr;
        return DcNavHarness::LoadMap(dir, MAP);
    }

    void Print(char const* what, DcNavHarness::RouteResult const& r)
    {
        std::printf("  %-34s reachable=%d complete=%d len2d=%.1f maxStepZ=%.1f pts=%zu %s\n", what,
                    r.reachable, r.corridorComplete, r.routeLength2d, r.maxStepZ, r.points.size(),
                    r.failureReason.c_str());
    }

    void Snap(dtNavMesh const* mesh, char const* what, float x, float y, float z)
    {
        G3D::Vector3 p;
        bool const ok = DcNavHarness::NearestPoint(mesh, x, y, z, 3.0f, 6.0f, p);
        float br, bc;
        DcChess::BoardCoords(x, y, br, bc);
        std::printf("  %-34s (%.2f, %.2f, %.2f) board(%.2f, %.2f) -> %s (%.2f, %.2f, %.2f)\n", what, x, y,
                    z, br, bc, ok ? "mesh" : "OFF-MESH", p.x, p.y, p.z);
    }
}

TEST(KarazhanChessProbe, PrintsTheHall)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset";

    std::printf("landmarks:\n");
    Snap(mesh.get(), "hall door 184276", -11120.0f, -1826.89f, 241.8f);
    Snap(mesh.get(), "waiting spot (release teleport)", -11106.92f, -1843.32f, 229.626f);
    Snap(mesh.get(), "Echo of Medivh", -11098.8f, -1853.56f, 221.15f);
    Snap(mesh.get(), "Dust Covered Chest", -11102.7f, -1848.98f, 221.07f);
    Snap(mesh.get(), "exit door 184277", -11048.2f, -1917.13f, 235.8f);
    Snap(mesh.get(), "Prince", -10962.2f, -2018.73f, 275.4f);
    Snap(mesh.get(), "Netherspite", -11134.0f, -1582.82f, 278.8f);

    std::printf("the col-0 edge, board c = -1.0 / -2.0 / -3.0:\n");
    for (float c : { -1.0f, -2.0f, -3.0f })
        for (float r = 0.0f; r <= 7.0f; r += 1.0f)
        {
            float x, y;
            DcChess::CenterOf(r, c, x, y);
            char label[64];
            std::snprintf(label, sizeof(label), "edge r%.0f c%.1f", r, c);
            Snap(mesh.get(), label, x, y, DcChess::Z + 0.5f);
        }

    std::printf("the other three edges, one cell off the board:\n");
    for (float t = 0.0f; t <= 7.0f; t += 1.0f)
    {
        float x, y;
        char label[64];
        DcChess::CenterOf(-1.0f, t, x, y);
        std::snprintf(label, sizeof(label), "row -1 c%.0f", t);
        Snap(mesh.get(), label, x, y, DcChess::Z + 0.5f);
        DcChess::CenterOf(8.0f, t, x, y);
        std::snprintf(label, sizeof(label), "row 8 c%.0f", t);
        Snap(mesh.get(), label, x, y, DcChess::Z + 0.5f);
        DcChess::CenterOf(t, 8.0f, x, y);
        std::snprintf(label, sizeof(label), "col 8 r%.0f", t);
        Snap(mesh.get(), label, x, y, DcChess::Z + 0.5f);
    }

    std::printf("routes:\n");
    float ex, ey;
    DcChess::CenterOf(3.45f, -2.5f, ex, ey);
    auto route = [&](char const* what, float sx, float sy, float sz, float tx, float ty, float tz)
    {
        DcNavHarness::RouteResult const r = DcNavHarness::Route(mesh.get(), MAP, sx, sy, sz, tx, ty, tz);
        Print(what, r);
        return r;
    };
    route("door -> Echo side", -11120.0f, -1826.89f, 241.8f, ex, ey, DcChess::Z);
    DcNavHarness::RouteResult const wait =
        route("waiting spot -> Echo side", -11106.92f, -1843.32f, 229.626f, ex, ey, DcChess::Z);
    for (G3D::Vector3 const& p : wait.points)
        std::printf("      (%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
    float cx, cy;
    DcChess::CenterOf(3.5f, 3.5f, cx, cy);
    route("Echo side -> board centre", ex, ey, DcChess::Z, cx, cy, DcChess::Z);
    route("Echo side -> exit door", ex, ey, DcChess::Z, -11048.2f, -1917.13f, 235.8f);
    DcNavHarness::RouteResult const prince =
        route("Echo side -> Prince", ex, ey, DcChess::Z, -10962.2f, -2018.73f, 275.4f);
    for (G3D::Vector3 const& p : prince.points)
        std::printf("      (%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
    DcNavHarness::RouteResult const nsp =
        route("Netherspite -> Echo side", -11134.0f, -1582.82f, 278.8f, ex, ey, DcChess::Z);
    for (G3D::Vector3 const& p : nsp.points)
        std::printf("      (%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
}

namespace
{
    constexpr float NETHERSPACE_DOOR[3] = { -11018.5f, -1967.92f, 276.652f };
    constexpr float PRINCE[3] = { -10962.2f, -2018.73f, 275.443f };

    struct Flood
    {
        int polys{0};
        float minX{1e9f}, maxX{-1e9f}, minY{1e9f}, maxY{-1e9f}, minZ{1e9f}, maxZ{-1e9f};
        std::vector<G3D::Vector3> centres;
    };

    dtPolyRef NearestRef(dtNavMesh const* mesh, float x, float y, float z)
    {
        dtNavMeshQuery* q = dtAllocNavMeshQuery();
        dtPolyRef ref = 0;
        if (q && dtStatusSucceed(q->init(mesh, 4096)))
        {
            dtQueryFilter f;
            float const pt[3] = { y, z, x };
            float const ext[3] = { 3.0f, 6.0f, 3.0f };
            float nearest[3];
            q->findNearestPoly(pt, ext, &f, &ref, nearest);
        }
        dtFreeNavMeshQuery(q);
        return ref;
    }

    // Every poly reachable from `start` without a poly whose centre is within
    // `cut` yards (2D) of the door — the door "shut".
    Flood FloodFrom(dtNavMesh const* mesh, dtPolyRef start, float cut, int cap)
    {
        Flood out;
        std::set<dtPolyRef> seen{ start };
        std::deque<dtPolyRef> todo{ start };
        while (!todo.empty() && out.polys < cap)
        {
            dtPolyRef const ref = todo.front();
            todo.pop_front();
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(ref, &tile, &poly)))
                continue;
            float cx = 0, cy = 0, cz = 0;
            for (int i = 0; i < poly->vertCount; ++i)
            {
                float const* v = &tile->verts[poly->verts[i] * 3];
                // Detour {y, z, x}
                cx += v[2];
                cy += v[0];
                cz += v[1];
                out.minX = std::min(out.minX, v[2]);
                out.maxX = std::max(out.maxX, v[2]);
                out.minY = std::min(out.minY, v[0]);
                out.maxY = std::max(out.maxY, v[0]);
                out.minZ = std::min(out.minZ, v[1]);
                out.maxZ = std::max(out.maxZ, v[1]);
            }
            cx /= poly->vertCount;
            cy /= poly->vertCount;
            cz /= poly->vertCount;
            out.centres.emplace_back(cx, cy, cz);
            ++out.polys;
            for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
            {
                dtPolyRef const nb = tile->links[k].ref;
                if (!nb || seen.count(nb))
                    continue;
                seen.insert(nb);
                dtMeshTile const* nt = nullptr;
                dtPoly const* np = nullptr;
                if (dtStatusFailed(mesh->getTileAndPolyByRef(nb, &nt, &np)))
                    continue;
                float nx = 0, ny = 0;
                for (int i = 0; i < np->vertCount; ++i)
                {
                    nx += nt->verts[np->verts[i] * 3 + 2];
                    ny += nt->verts[np->verts[i] * 3 + 0];
                }
                nx /= np->vertCount;
                ny /= np->vertCount;
                if (std::hypot(nx - NETHERSPACE_DOOR[0], ny - NETHERSPACE_DOOR[1]) < cut)
                    continue;
                todo.push_back(nb);
            }
        }
        return out;
    }
}

// Netherspace (Prince Malchezaar's platform): the Netherspace Door (185134) is a
// DOOR_TYPE_ROOM door on DATA_MALCHEZAAR, shut for the whole fight. The sealed
// row needs the room's volume with the door cut, and to know that no corridor
// poly falls inside it.
TEST(KarazhanChessProbe, PrintsNetherspace)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset";

    for (float cut : { 3.0f, 5.0f, 8.0f })
    {
        Flood const room = FloodFrom(mesh.get(), NearestRef(mesh.get(), PRINCE[0], PRINCE[1], PRINCE[2]), cut, 5000);
        std::printf("room (door cut %.0fyd): %d polys, x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f\n", cut,
                    room.polys, room.minX, room.maxX, room.minY, room.maxY, room.minZ, room.maxZ);
    }
    // The corridor side: from the stair landing just outside the door.
    Flood const hall = FloodFrom(mesh.get(), NearestRef(mesh.get(), -11056.02f, -1966.30f, 274.73f), 5.0f, 400);
    std::printf("corridor (400 polys from the landing): x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f\n", hall.minX,
                hall.maxX, hall.minY, hall.maxY, hall.minZ, hall.maxZ);
    int inside = 0;
    for (G3D::Vector3 const& c : hall.centres)
        if (c.x > NETHERSPACE_DOOR[0] + 2.0f && c.z > 265.0f && c.z < 290.0f)
        {
            ++inside;
            std::printf("  corridor poly east of the door: (%.1f, %.1f, %.1f)\n", c.x, c.y, c.z);
        }
    std::printf("corridor polys east of the door line: %d\n", inside);
    std::printf("door -> Prince %.1fyd\n",
                std::hypot(PRINCE[0] - NETHERSPACE_DOOR[0], PRINCE[1] - NETHERSPACE_DOOR[1]));
}
