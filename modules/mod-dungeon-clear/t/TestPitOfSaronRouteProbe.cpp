/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Pit of Saron (map 658) route through
// the Ymirjar gauntlet and the icicle tunnel, Krick -> Tyrannus's ledge ->
// Scourgelord Tyrannus.
//
// WHAT THIS IS A GATE ON, and why it is not a nicety.
//
// Three of this dungeon's four set pieces are fired by a bot standing inside an
// areatrigger SPHERE and sending CMSG_AREATRIGGER, and the core re-tests
// Player::IsInAreaTriggerRadius on the packet — a 3D distance against the DBC
// centre. So every one of the driver's stand points carries TWO invariants that
// are invisible from the source:
//
//   1. it has to be somewhere a bot can actually stand (on the navmesh), and
//   2. it has to be INSIDE its own sphere, or the forge is a silent no-op and the
//      party stands in a corridor forever.
//
// The ledge anchor carries the mirror of (2): it must be OUTSIDE areatrigger
// 5633's sphere, because the whole design of event 2 is that ARRIVING at the
// objective is not the same act as ENTERING the encounter.
//
// The suite also PRINTS the routed polylines. That is deliberate: this is the
// tool the anchors were authored with (route the leg, decimate the polyline —
// [[dc-navharness-prints-the-route]]), so an mmaps regen that moves a corridor is
// re-authored the same way instead of by hand.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, same contract as the Azjol-Nerub,
// Blackwing Lair, Halls of Lightning, Mechanar, Ramparts and Utgarde Pinnacle
// probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='PitOfSaronRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace DcPitOfSaron;

    struct Pt { float x, y, z; char const* name; };

    // The entrance is DcTestDungeonRegistry's map-658 row — the
    // areatrigger_teleport target a walked-in party lands on. The two derived
    // bosses are their live `creature` rows. Tyrannus has no spawn at all; his
    // point is the exitPos boss_tyrannusAI::DoAction(1) MoveJump()s him to, which
    // is also the roster anchor this file asserts.
    constexpr Pt ENTRANCE = {  435.74f,  212.41f, 528.71f, "entrance"          };
    constexpr Pt GARFROST = { GARFROST_X, GARFROST_Y, GARFROST_Z, "Garfrost"   };
    constexpr Pt ICK      = { ICK_X,    ICK_Y,    ICK_Z,    "Krick and Ick"    };
    constexpr Pt LEDGE    = { LEDGE_X,  LEDGE_Y,  LEDGE_Z,  "Tyrannus's ledge" };
    constexpr Pt TYRANNUS = { TYRANNUS_X, TYRANNUS_Y, TYRANNUS_Z, "Tyrannus"   };

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this anchor is not standing anywhere near here" rather than "something was
    // found across the ravine"; vertical covers the ordinary float between an
    // authored z and the mesh surface under it.
    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 6.0f;

    // How far the mesh may move an authored anchor before it stops being an
    // authored anchor. A couple of yards is ordinary detail-mesh float; more than
    // that and the point was written somewhere the party cannot stand.
    constexpr float SNAP_TOLERANCE = 3.0f;

    // Largest vertical step between consecutive corridor points that is still a
    // WALK. Anything above this is a ledge the party would have to fall down, and
    // no leg on this route has one.
    constexpr float MAX_STEP_Z = 6.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 658";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-658 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    std::vector<WaypointHint> const* GauntletRoute()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL,
                                              BossRosterRegistry::ObjectiveEntry(1));
    }

    std::vector<WaypointHint> const* ArenaRoute()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_TYRANNUS);
    }

    // Route a leg and print its polyline. The printing IS the authoring tool.
    DcNavHarness::RouteResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("\n=== Pit of Saron (658): %s -> %s ===\n", a.name, b.name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                    r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                    r.maxStepZ, r.failureReason.c_str());
        for (std::size_t i = 0; i < r.points.size(); ++i)
            std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z);
        return r;
    }

    float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    void SnapCheck(dtNavMesh const* mesh, char const* what, float x, float y, float z)
    {
        G3D::Vector3 snapped;
        bool const ok = DcNavHarness::NearestPoint(mesh, x, y, z, SNAP_H, SNAP_V, snapped);
        float const d = ok ? Dist3(x, y, z, snapped.x, snapped.y, snapped.z) : -1.0f;

        std::printf("  [%-22s] (%8.2f, %9.2f, %7.2f)  ->  ", what, x, y, z);
        if (ok)
            std::printf("(%8.2f, %9.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, d);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << what << " at (" << x << ", " << y << ", " << z
                        << ") is off the navmesh — a bot cannot stand there, so it can neither "
                           "walk to it nor forge an areatrigger from it";
        if (ok)
            EXPECT_LT(d, SNAP_TOLERANCE)
                << what << " snapped " << d << "yd — it is not standing where it was authored";
    }
}

// THE INVARIANT THAT MAKES THE DUNGEON WORK: each of the three gate stand points
// is inside the sphere it fires, and the ledge anchor is outside the one it must
// not fire.
//
// This needs no navmesh and is therefore the one case here that always runs.
TEST(PitOfSaronRouteProbe, GateStandPointsAreInsideTheirOwnSpheresAndTheLedgeIsOutside)
{
    struct Gate
    {
        char const* name;
        uint32 trigger;
        float sx, sy, sz;   // the stand point
        float cx, cy, cz, r;  // the DBC sphere
    };
    static constexpr Gate kGates[] = {
        { "gate 1", AREATRIGGER_WARN_1, GATE_1_X, GATE_1_Y, GATE_1_Z,
          AT_WARN_1_X, AT_WARN_1_Y, AT_WARN_1_Z, AT_WARN_1_R },
        { "gate 2", AREATRIGGER_WARN_2, GATE_2_X, GATE_2_Y, GATE_2_Z,
          AT_WARN_2_X, AT_WARN_2_Y, AT_WARN_2_Z, AT_WARN_2_R },
        { "gate 3", AREATRIGGER_TUNNEL, GATE_3_X, GATE_3_Y, GATE_3_Z,
          AT_TUNNEL_X, AT_TUNNEL_Y, AT_TUNNEL_Z, AT_TUNNEL_R },
        { "the arena stand point", AREATRIGGER_TYRANNUS, ARENA_X, ARENA_Y, ARENA_Z,
          AT_TYRANNUS_X, AT_TYRANNUS_Y, AT_TYRANNUS_Z, AT_TYRANNUS_R },
    };

    for (Gate const& g : kGates)
    {
        float const d = Dist3(g.sx, g.sy, g.sz, g.cx, g.cy, g.cz);
        // The GATE_LEASH is how far off the stand point the leader may settle and
        // still count as arrived, so the margin has to cover it — an arrival on
        // the far side of the leash must still be inside the sphere.
        EXPECT_LT(d + GATE_LEASH, g.r)
            << g.name << ": the stand point is " << d << "yd from areatrigger " << g.trigger
            << "'s centre and its radius is " << g.r
            << ". Player::IsInAreaTriggerRadius takes its radius>0 branch for this trigger, so a"
               " forge from outside is a SILENT no-op and the party never advances.";
    }

    // And the mirror. The ledge is the objective anchor and the gather point; if
    // it were inside 5633's sphere the party would trip the encounter on arrival,
    // strung out down the tunnel, with the gather that exists to prevent exactly
    // that never having run.
    float const ledge = Dist3(LEDGE_X, LEDGE_Y, LEDGE_Z,
                              AT_TYRANNUS_X, AT_TYRANNUS_Y, AT_TYRANNUS_Z);
    EXPECT_GT(ledge, AT_TYRANNUS_R + LEDGE_ARRIVE)
        << "the ledge anchor is " << ledge << "yd from areatrigger " << AREATRIGGER_TYRANNUS
        << "'s centre (radius " << AT_TYRANNUS_R
        << "): arriving there would START the Tyrannus encounter, which is the one thing this"
           " objective exists to make deliberate";

    // The arena stand point also has to be well inside the boss's own leash box,
    // because the encounter's first act is AttackStart on the nearest player and
    // the leash is checked against the VICTIM every tick.
    float const leash = Dist3(ARENA_X, ARENA_Y, ARENA_Z, LEASH_X, LEASH_Y, LEASH_Z);
    EXPECT_LT(leash, LEASH_RADIUS * 0.5f)
        << "the arena stand point is " << leash << "yd from TSDistCheckPos; the boss evades at "
        << LEASH_RADIUS << "yd and the party must have room to fight, not just to stand";
    EXPECT_LT(std::fabs(ARENA_Z - LEASH_Z), LEASH_ZBAND * 0.5f)
        << "the arena stand point is outside half the boss's vertical leash band";
}

// The staging point the ARM state holds the party on must be clear of gate 1's
// sphere on every bearing, or the hold is not a hold at all.
TEST(PitOfSaronRouteProbe, TheArmStagingPointIsClearOfGateOne)
{
    float const d = Dist3(STAGE_X, STAGE_Y, STAGE_Z, AT_WARN_1_X, AT_WARN_1_Y, AT_WARN_1_Z);
    EXPECT_GT(d, AT_WARN_1_R + STAGE_LEASH + 20.0f)
        << "the ARM staging point is only " << d << "yd from areatrigger " << AREATRIGGER_WARN_1
        << " (radius " << AT_WARN_1_R
        << "). The whole point of that state is to keep the party OUT of the ambush trigger for"
           " the 85-100s the Krick outro needs to fly the orchestrator into position.";
}

// Every authored anchor must be somewhere a bot can actually stand.
TEST(PitOfSaronRouteProbe, EveryAnchorIsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("=== Pit of Saron (658) — the driver's own points ===\n");
    SnapCheck(mesh.get(), "ARM staging",   STAGE_X,  STAGE_Y,  STAGE_Z);
    SnapCheck(mesh.get(), "gate 1 stand",  GATE_1_X, GATE_1_Y, GATE_1_Z);
    SnapCheck(mesh.get(), "gate 2 stand",  GATE_2_X, GATE_2_Y, GATE_2_Z);
    SnapCheck(mesh.get(), "gate 3 stand",  GATE_3_X, GATE_3_Y, GATE_3_Z);
    SnapCheck(mesh.get(), "the ledge",     LEDGE_X,  LEDGE_Y,  LEDGE_Z);
    SnapCheck(mesh.get(), "arena stand",   ARENA_X,  ARENA_Y,  ARENA_Z);
    SnapCheck(mesh.get(), "Tyrannus exit", TYRANNUS_X, TYRANNUS_Y, TYRANNUS_Z);

    std::printf("=== Pit of Saron (658) — the authored routes ===\n");
    std::vector<WaypointHint> const* gauntlet = GauntletRoute();
    ASSERT_NE(gauntlet, nullptr) << "no authored Krick -> ledge route on map 658";
    for (std::size_t i = 0; i < gauntlet->size(); ++i)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "leg A anchor %zu", i);
        SnapCheck(mesh.get(), buf, (*gauntlet)[i].x, (*gauntlet)[i].y, (*gauntlet)[i].z);
    }

    std::vector<WaypointHint> const* arena = ArenaRoute();
    ASSERT_NE(arena, nullptr) << "no authored ledge -> Tyrannus route on map 658";
    for (std::size_t i = 0; i < arena->size(); ++i)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "leg B anchor %zu", i);
        SnapCheck(mesh.get(), buf, (*arena)[i].x, (*arena)[i].y, (*arena)[i].z);
    }
    std::printf("====================================================\n");
}

// The authored row's shape, pinned without a navmesh: it starts where Ick dies,
// ends on the ledge, passes through all three gate stand points in order, and
// carries NO_STOP from the gate-1 crossing to the far end of the icicle tunnel.
//
// A regen that re-derives this row has to keep all four properties. Losing the
// gate anchors means the ordinary clear walks the corridor A* prefers, which
// never comes closer than 33.2yd to areatrigger 5578's 34.54yd sphere; losing the
// NO_STOP span means the pull drags a camp back out of the volume the driver
// measures wave 2 in.
TEST(PitOfSaronRouteProbe, TheGauntletRowRunsThroughEveryGateAndCarriesNoStop)
{
    std::vector<WaypointHint> const* route = GauntletRoute();
    ASSERT_NE(route, nullptr) << "no authored Krick -> ledge route on map 658";
    ASSERT_GT(route->size(), 10u);

    auto indexOf = [&](float x, float y) -> int
    {
        for (std::size_t i = 0; i < route->size(); ++i)
            if (std::fabs((*route)[i].x - x) < 0.01f && std::fabs((*route)[i].y - y) < 0.01f)
                return int(i);
        return -1;
    };

    // It starts on the ground the party is standing on when Ick dies. SeedCursor
    // projects the leader onto the row from where it stands, and a row that starts
    // somewhere else snaps the cursor to the far end.
    EXPECT_NEAR(route->front().x, ICK_X, 0.01f);
    EXPECT_NEAR(route->front().y, ICK_Y, 0.01f);
    EXPECT_NEAR(route->back().x, LEDGE_X, 0.01f);
    EXPECT_NEAR(route->back().y, LEDGE_Y, 0.01f);

    int const stage = indexOf(STAGE_X, STAGE_Y);
    int const g1 = indexOf(GATE_1_X, GATE_1_Y);
    int const g2 = indexOf(GATE_2_X, GATE_2_Y);
    int const g3 = indexOf(GATE_3_X, GATE_3_Y);
    ASSERT_GE(stage, 0) << "the ARM staging point is not on the authored row";
    ASSERT_GE(g1, 0) << "gate 1's stand point is not on the authored row — the ordinary clear "
                        "would walk the corridor A* prefers, which only grazes the sphere";
    ASSERT_GE(g2, 0) << "gate 2's stand point is not on the authored row";
    ASSERT_GE(g3, 0) << "gate 3's stand point is not on the authored row";
    EXPECT_LT(stage, g1);
    EXPECT_LT(g1, g2);
    EXPECT_LT(g2, g3) << "the three gates are strictly ordered and so is the row";

    // NO_STOP opens at the gate-1 crossing and has not closed by gate 3.
    EXPECT_TRUE(HasFlag((*route)[g1].flags, AnchorFlag::NO_STOP))
        << "the leg leaving gate 1 must be crossed without a pull";
    EXPECT_TRUE(HasFlag((*route)[g2].flags, AnchorFlag::NO_STOP));
    EXPECT_TRUE(HasFlag((*route)[g3].flags, AnchorFlag::NO_STOP));
    EXPECT_FALSE(HasFlag((*route)[stage].flags, AnchorFlag::NO_STOP))
        << "the approach from Krick is ordinary ground; the party may pull on it";
    EXPECT_FALSE(HasFlag(route->back().flags, AnchorFlag::NO_STOP))
        << "the ledge approach is out of the icicle field and must release the span";

    // ...and the span is CONTIGUOUS. A hole in the middle is worse than no span
    // at all: the pull re-arms for exactly the anchors it was meant to skip.
    int first = -1, last = -1;
    for (std::size_t i = 0; i < route->size(); ++i)
        if (HasFlag((*route)[i].flags, AnchorFlag::NO_STOP))
        {
            if (first < 0)
                first = int(i);
            last = int(i);
        }
    ASSERT_GE(first, 0);
    for (int i = first; i <= last; ++i)
        EXPECT_TRUE(HasFlag((*route)[i].flags, AnchorFlag::NO_STOP))
            << "anchor " << i << " is a hole in the NO_STOP span";
    EXPECT_EQ(first, g1);

    // No step so long that the escort's straight-leg walk stops following the
    // corridor it was decimated from. This leg climbs 125yd in ramps, so the
    // vertical bound is generous and the horizontal one is not.
    for (std::size_t i = 1; i < route->size(); ++i)
    {
        WaypointHint const& a = (*route)[i - 1];
        WaypointHint const& b = (*route)[i];
        float const flat = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
        EXPECT_LT(flat, 22.0f) << "anchor " << i << " is " << flat
                               << "yd of ground from the last one — too long to still be "
                                  "following the corridor it was decimated from";
        EXPECT_LT(std::fabs(a.z - b.z), 13.0f)
            << "anchor " << i << " climbs " << std::fabs(a.z - b.z) << "yd in one step";
    }
}

// Leg B has to put the driver's arena stand point ON the walked line, so the
// ordinary clear and hook 30 take the same way into the encounter — and every
// anchor of it has to be inside the boss's leash box, which is checked against his
// VICTIM every tick from the moment the trigger fires.
TEST(PitOfSaronRouteProbe, TheArenaLegStaysInsideTyrannusLeashBox)
{
    std::vector<WaypointHint> const* route = ArenaRoute();
    ASSERT_NE(route, nullptr) << "no authored ledge -> Tyrannus route on map 658";
    ASSERT_GE(route->size(), 3u);

    EXPECT_NEAR(route->front().x, LEDGE_X, 0.01f);
    EXPECT_NEAR(route->back().x, TYRANNUS_X, 0.01f);
    EXPECT_NEAR(route->back().y, TYRANNUS_Y, 0.01f);

    bool sawArena = false;
    for (WaypointHint const& h : *route)
    {
        if (std::fabs(h.x - ARENA_X) < 0.01f && std::fabs(h.y - ARENA_Y) < 0.01f)
            sawArena = true;

        // The ledge itself is outside the leash box's comfort margin by design —
        // it is the gather point, 73yd from the trigger — so only measure from the
        // arena stand point on.
        if (!sawArena)
            continue;

        float const d = Dist3(h.x, h.y, h.z, LEASH_X, LEASH_Y, LEASH_Z);
        EXPECT_LT(d, LEASH_RADIUS * 0.6f)
            << "an anchor inside the arena is " << d
            << "yd from TSDistCheckPos; the boss full-heals and evades at " << LEASH_RADIUS;
        EXPECT_LT(std::fabs(h.z - LEASH_Z), LEASH_ZBAND * 0.5f);
    }
    EXPECT_TRUE(sawArena)
        << "the driver's arena stand point is not on the authored leg — hook 30 and the "
           "ordinary clear would take different lines into a one-way encounter";
}

// THE WHOLE DUNGEON, end to end. Five legs, and the printing of them is what the
// authored anchors above were decimated from.
TEST(PitOfSaronRouteProbe, TheDungeonIsOneContinuousWalk)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    DcNavHarness::RouteResult const a = PrintLeg(mesh.get(), ENTRANCE, GARFROST);
    DcNavHarness::RouteResult const b = PrintLeg(mesh.get(), GARFROST, ICK);
    DcNavHarness::RouteResult const c = PrintLeg(mesh.get(), ICK, LEDGE);
    DcNavHarness::RouteResult const d = PrintLeg(mesh.get(), LEDGE, TYRANNUS);
    std::printf("\n");

    // ...and the gauntlet, split at the three gates, because those are the points
    // the driver actually walks between and a corridor that is continuous end to
    // end can still be discontinuous at a stand point placed off it.
    Pt const g1 = { GATE_1_X, GATE_1_Y, GATE_1_Z, "gate 1" };
    Pt const g2 = { GATE_2_X, GATE_2_Y, GATE_2_Z, "gate 2" };
    Pt const g3 = { GATE_3_X, GATE_3_Y, GATE_3_Z, "gate 3" };
    Pt const stage = { STAGE_X, STAGE_Y, STAGE_Z, "ARM staging" };

    DcNavHarness::RouteResult const s0 = PrintLeg(mesh.get(), ICK, stage);
    DcNavHarness::RouteResult const s1 = PrintLeg(mesh.get(), stage, g1);
    DcNavHarness::RouteResult const s2 = PrintLeg(mesh.get(), g1, g2);
    DcNavHarness::RouteResult const s3 = PrintLeg(mesh.get(), g2, g3);
    DcNavHarness::RouteResult const s4 = PrintLeg(mesh.get(), g3, LEDGE);
    DcNavHarness::RouteResult const s5 = PrintLeg(mesh.get(), LEDGE, { ARENA_X, ARENA_Y, ARENA_Z, "arena" });
    std::printf("\n");

    for (auto const* leg : { &a, &b, &c, &d, &s0, &s1, &s2, &s3, &s4, &s5 })
    {
        EXPECT_TRUE(leg->reachable);
        EXPECT_TRUE(leg->corridorComplete);
        EXPECT_LT(leg->maxStepZ, MAX_STEP_Z)
            << "a corridor contains a " << leg->maxStepZ << "yd vertical step — that is a ledge";
    }
}
