/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Culling of Stratholme (map 595)
// anchors and for the legs the wave controller walks.
//
// TWO THINGS THIS IS A GATE ON, and neither is a nicety on this map.
//
// 1. MAP 595 HAS A FLAT NAVMESH SHEET AT z = 0.14 UNDER MOST OF THE CITY. It is
//    one of the twelve maps with that shape, and the consequence is that an anchor
//    written a few yards off a walkable surface does NOT fail loudly: a bot
//    commanded there resolves to the sheet and sinks about a hundred and thirty
//    yards through the floor. Every authored point therefore has to be proven to
//    snap to the surface it was written for, with a tight tolerance.
//
// 2. FOUR CITY GATES ARE PERMANENTLY SHUT AND THE NAVMESH CANNOT SEE THEM. The two
//    Market Row Gates (187579, 187712) and the two Festival Lane Gates (188685,
//    188687) are GAMEOBJECT_TYPE_DOOR with lockId 0, startOpen 0, no ScriptName,
//    no AIName, no SmartAI row and no entry in instance_culling_of_stratholme's
//    GUID store. Nothing in the core ever opens one. But mmaps are baked from
//    static geometry only — a shut door is a runtime GameObject — so Detour routes
//    straight through them, and a lock-free door is one
//    BotCanOpenDoorLikePlayer REFUSES, which means the blocking-door machinery
//    would auto-pause the run in front of a gate that can never open.
//
//    So every leg's polyline is checked against all four. A leg that threads one is
//    a leg the party cannot actually walk, and the fix is a route or an anchor —
//    never a door row, because these gates really do block.
//
// The suite also PRINTS the routed polylines, deliberately: this is the tool the
// anchors were authored with, so an mmaps regen that moves a street is re-authored
// the same way instead of by hand.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, the same contract as the Azjol-Nerub,
// Blackwing Lair, Halls of Lightning, Mechanar, Ramparts, Utgarde Pinnacle, Pit of
// Saron and Halls of Reflection probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='CullingOfStratholmeRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "MapDefines.h"

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
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
    using namespace DcCullingOfStratholme;

    struct Pt { float x, y, z; char const* name; };

    // The entrance is DcTestDungeonRegistry's map-595 row — the areatrigger_teleport
    // target a walked-in party lands on.
    constexpr Pt ENTRANCE = { 1431.10f, 556.92f, 36.69f, "entrance" };

    constexpr Pt CHROMIE     = { CHROMIE_X, CHROMIE_Y, CHROMIE_Z, "Chromie (entrance)" };
    constexpr Pt CHROMIE_MID = { CHROMIE_MID_X, CHROMIE_MID_Y, CHROMIE_MID_Z, "Chromie (middle)" };
    constexpr Pt BRIDGE      = { BRIDGE_X, BRIDGE_Y, BRIDGE_Z, "Arthas WP0 (bridge)" };
    constexpr Pt MARKET_ROW  = { MARKET_ROW_X, MARKET_ROW_Y, MARKET_ROW_Z, "Market Row" };
    constexpr Pt CORRUPTOR   = { CORRUPTOR_X, CORRUPTOR_Y, CORRUPTOR_Z, "Infinite Corruptor" };
    constexpr Pt TOWN_HALL   = { TOWN_HALL_X, TOWN_HALL_Y, TOWN_HALL_Z, "Town Hall door (WP20)" };
    constexpr Pt TH_MEET     = { TOWN_HALL_MEET_X, TOWN_HALL_MEET_Y, TOWN_HALL_MEET_Z,
                                 "Arthas WP16 (Town Hall leg meeting point)" };
    // Arthas's WP11 wave stop (= LeaderIntroPos2special). Not an authored anchor:
    // it is here because the meeting point's whole job is to be within grid
    // activation range of it, which the leg test measures.
    constexpr Pt ARTHAS_STOP = { 2092.15f, 1276.65f, 140.52f, "Arthas WP11 (wave stop)" };
    constexpr Pt PASSAGE     = { PASSAGE_X, PASSAGE_Y, PASSAGE_Z, "Arthas WP31 (post-Epoch)" };
    constexpr Pt MARKET      = { MARKET_X, MARKET_Y, MARKET_Z, "Arthas WP54 (pre-Mal'ganis)" };

    // The four wave clusters. Documentation constants in the events table; here they
    // are the route endpoints, because what the driver actually walks to is a live
    // mob standing within ~10yd of one of these.
    constexpr Pt KS = { CLUSTER_KS_X, CLUSTER_KS_Y, CLUSTER_KS_Z, "King's Square (waves 1/6/9)" };
    constexpr Pt FL = { CLUSTER_FL_X, CLUSTER_FL_Y, CLUSTER_FL_Z, "Festival Lane (wave 2)" };
    constexpr Pt MR = { CLUSTER_MR_X, CLUSTER_MR_Y, CLUSTER_MR_Z, "Market Row (waves 3/7 + bosses)" };
    constexpr Pt ES = { CLUSTER_ES_X, CLUSTER_ES_Y, CLUSTER_ES_Z, "Elders' Square (waves 4/8)" };

    // The four PERMANENTLY SHUT city gates. See the header: nothing opens them and
    // the navmesh cannot see them, so a route through one is a route the party
    // cannot walk.
    struct Gate { uint32 entry; float x, y, z; char const* name; };
    constexpr Gate kShutGates[] = {
        { 187579, 2235.83f, 1331.63f, 126.05f, "Market Row Gate (west)"   },
        { 187712, 2256.25f, 1331.31f, 124.20f, "Market Row Gate (east)"   },
        { 188685, 2349.09f, 1227.32f, 130.27f, "Festival Lane Gate (south)" },
        { 188687, 2342.43f, 1248.43f, 131.51f, "Festival Lane Gate (north)" },
    };

    // How close a corridor point may come to a shut gate's origin before the leg is
    // judged to thread it. A door model is a few yards wide; 6yd in 2D with a
    // floor band is comfortably inside the doorway and comfortably outside the
    // street beside it.
    constexpr float GATE_CLEARANCE_2D = 6.0f;
    constexpr float GATE_CLEARANCE_Z  = 10.0f;

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this anchor is not standing anywhere near here"; vertical covers the float
    // between an authored z and the detail mesh under it — and has to stay well
    // under the ~130yd drop to the z = 0.14 sheet, which is the whole point.
    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 6.0f;
    constexpr float SNAP_TOLERANCE = 3.0f;

    // Largest vertical step between consecutive corridor points that is still a
    // WALK. The city is terraced but every street is walkable; a bigger step than
    // this is a drop.
    constexpr float MAX_STEP_Z = 8.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 595";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-595 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Route a leg, print its polyline, and report the closest approach it makes to
    // any permanently shut gate. The printing IS the authoring tool.
    struct LegResult
    {
        DcNavHarness::RouteResult route;
        float  gateNearest = 1e9f;
        Gate const* gate = nullptr;
    };

    LegResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        LegResult out;
        out.route = DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        DcNavHarness::RouteResult const& r = out.route;

        std::printf("\n=== Culling of Stratholme (595): %s -> %s ===\n", a.name, b.name);
        std::printf("  reachable=%d complete=%d pts=%u len2d=%.1f maxStepZ=%.2f %s\n",
                    r.reachable, r.corridorComplete, r.pointCount, r.routeLength2d,
                    r.maxStepZ, r.failureReason.c_str());
        for (std::size_t i = 0; i < r.points.size(); ++i)
            std::printf("  [pt %3zu] %9.2ff, %9.2ff, %7.2ff\n",
                        i, r.points[i].x, r.points[i].y, r.points[i].z);

        for (G3D::Vector3 const& p : r.points)
            for (Gate const& g : kShutGates)
            {
                float const d2 = std::hypot(p.x - g.x, p.y - g.y);
                if (std::fabs(p.z - g.z) > GATE_CLEARANCE_Z)
                    continue;
                if (d2 < out.gateNearest)
                {
                    out.gateNearest = d2;
                    out.gate = &g;
                }
            }

        if (out.gate)
            std::printf("  closest shut gate: %s (%u) at %.2fyd\n",
                        out.gate->name, out.gate->entry, out.gateNearest);
        else
            std::printf("  closest shut gate: none within the floor band\n");
        return out;
    }

    void ExpectClearOfShutGates(LegResult const& leg, char const* what)
    {
        if (!leg.gate)
            return;
        EXPECT_GT(leg.gateNearest, GATE_CLEARANCE_2D)
            << what << " routes within " << leg.gateNearest << "yd of " << leg.gate->name
            << " (" << leg.gate->entry << "), which is permanently SHUT and which the"
               " navmesh cannot see. A lock-free door is one BotCanOpenDoorLikePlayer"
               " refuses, so the party would auto-pause in front of a gate that never"
               " opens. Re-author the leg, not the door registry.";
    }

    void SnapCheck(dtNavMesh const* mesh, char const* what, float x, float y, float z)
    {
        G3D::Vector3 snapped;
        bool const ok = DcNavHarness::NearestPoint(mesh, x, y, z, SNAP_H, SNAP_V, snapped);
        float const d = ok ? Dist3(x, y, z, snapped.x, snapped.y, snapped.z) : -1.0f;

        std::printf("  [%-30s] (%8.2f, %9.2f, %7.2f)  ->  ", what, x, y, z);
        if (ok)
            std::printf("(%8.2f, %9.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, d);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << what << " at (" << x << ", " << y << ", " << z
                        << ") is off the navmesh. On map 595 that is not a harmless"
                           " authoring slip: there is a flat mesh sheet at z = 0.14 under"
                           " the city, and a bot sent to an off-mesh point resolves to it"
                           " and sinks ~130yd.";
        if (ok)
            EXPECT_LT(d, SNAP_TOLERANCE)
                << what << " snapped " << d << "yd — it is not standing where it was authored";
    }
}

// Every authored anchor, crate and cluster is somewhere a bot can stand.
TEST(CullingOfStratholmeRouteProbe, EveryAuthoredPointIsOnTheNavmesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Culling of Stratholme (595): authored points on the mesh ===\n");
    SnapCheck(mesh.get(), ENTRANCE.name, ENTRANCE.x, ENTRANCE.y, ENTRANCE.z);
    SnapCheck(mesh.get(), CHROMIE.name, CHROMIE.x, CHROMIE.y, CHROMIE.z);
    for (std::size_t i = 0; i < 5; ++i)
    {
        char label[32];
        std::snprintf(label, sizeof(label), "crate %zu", i + 1);
        SnapCheck(mesh.get(), label, CRATES[i].x, CRATES[i].y, CRATES[i].z);
    }
    SnapCheck(mesh.get(), CHROMIE_MID.name, CHROMIE_MID.x, CHROMIE_MID.y, CHROMIE_MID.z);
    SnapCheck(mesh.get(), BRIDGE.name, BRIDGE.x, BRIDGE.y, BRIDGE.z);
    SnapCheck(mesh.get(), KS.name, KS.x, KS.y, KS.z);
    SnapCheck(mesh.get(), FL.name, FL.x, FL.y, FL.z);
    SnapCheck(mesh.get(), MR.name, MR.x, MR.y, MR.z);
    SnapCheck(mesh.get(), ES.name, ES.x, ES.y, ES.z);
    SnapCheck(mesh.get(), MARKET_ROW.name, MARKET_ROW.x, MARKET_ROW.y, MARKET_ROW.z);
    SnapCheck(mesh.get(), CORRUPTOR.name, CORRUPTOR.x, CORRUPTOR.y, CORRUPTOR.z);
    SnapCheck(mesh.get(), TOWN_HALL.name, TOWN_HALL.x, TOWN_HALL.y, TOWN_HALL.z);
    SnapCheck(mesh.get(), TH_MEET.name, TH_MEET.x, TH_MEET.y, TH_MEET.z);
    SnapCheck(mesh.get(), ARTHAS_STOP.name, ARTHAS_STOP.x, ARTHAS_STOP.y, ARTHAS_STOP.z);
    SnapCheck(mesh.get(), PASSAGE.name, PASSAGE.x, PASSAGE.y, PASSAGE.z);
    SnapCheck(mesh.get(), MARKET.name, MARKET.x, MARKET.y, MARKET.z);
}

// The approach: the entrance, Chromie, the five crates in road order, and the long
// haul to Chromie-middle and the bridge. All of it is ordinary boss-nav between
// objective anchors, so all of it has to route.
TEST(CullingOfStratholmeRouteProbe, TheApproachAndTheCrateRoadRoute)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<Pt> chain;
    chain.push_back(ENTRANCE);
    chain.push_back(CHROMIE);
    static char labels[5][16];
    for (std::size_t i = 0; i < 5; ++i)
    {
        std::snprintf(labels[i], sizeof(labels[i]), "crate %zu", i + 1);
        chain.push_back({ CRATES[i].x, CRATES[i].y, CRATES[i].z, labels[i] });
    }
    chain.push_back(CHROMIE_MID);
    chain.push_back(BRIDGE);

    for (std::size_t i = 0; i + 1 < chain.size(); ++i)
    {
        LegResult const leg = PrintLeg(mesh.get(), chain[i], chain[i + 1]);
        EXPECT_TRUE(leg.route.reachable)
            << chain[i].name << " -> " << chain[i + 1].name << " does not route: "
            << leg.route.failureReason;
        EXPECT_TRUE(leg.route.corridorComplete)
            << chain[i].name << " -> " << chain[i + 1].name
            << " routes but the corridor stops short of the target poly";
        EXPECT_LT(leg.route.maxStepZ, MAX_STEP_Z)
            << chain[i].name << " -> " << chain[i + 1].name << " has a "
            << leg.route.maxStepZ << "yd vertical step — that is a drop, not a walk";
        ExpectClearOfShutGates(leg, chain[i].name);
    }
}

// THE INVARIANT THE WAVE CONTROLLER RESTS ON: every wave cluster is reachable from
// every other one, on foot, without passing a shut gate.
//
// The driver walks the tank to a LIVE MOB's own position rather than to any of
// these points, so these are proxies — but they are the right proxies: each wave
// spawns its four mobs within ~10yd of its cluster centroid, and if the centroid
// is unreachable the mobs are too. The expected cluster order across the ten waves
// is KS, FL, MR, ES, MR(Meathook), KS, MR, ES, KS, MR(Salramm), so every
// consecutive pair in that sequence is a leg the party really walks.
TEST(CullingOfStratholmeRouteProbe, EveryWaveClusterPairIsMutuallyReachable)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    // Arthas's stop at waypoint 11, where the party starts the wave phase.
    constexpr Pt WP11 = { 2092.15f, 1276.65f, 140.52f, "Arthas WP11 (city intro end)" };

    std::vector<Pt> const legsFrom = { WP11, KS, FL, MR, ES, MR, KS, MR, ES, KS };
    std::vector<Pt> const legsTo   = { KS,   FL, MR, ES, MR, KS, MR, ES, KS, MR };

    for (std::size_t i = 0; i < legsFrom.size(); ++i)
    {
        LegResult const leg = PrintLeg(mesh.get(), legsFrom[i], legsTo[i]);
        EXPECT_TRUE(leg.route.reachable)
            << "wave leg " << legsFrom[i].name << " -> " << legsTo[i].name
            << " does not route: " << leg.route.failureReason
            << ". A wave the party cannot reach is a wave counter that never moves.";
        EXPECT_TRUE(leg.route.corridorComplete)
            << "wave leg " << legsFrom[i].name << " -> " << legsTo[i].name
            << " routes but stops short of the target poly";
        ExpectClearOfShutGates(leg, "a wave leg");
    }
}

// The last third, walked in the order the run actually walks it: BACK west from
// Market Row (where the wave phase leaves the party) to Arthas's WP12, then east
// with him to the Town Hall door, then the post-Epoch anchor and his pre-Mal'ganis
// stop at the edge of the Market.
//
// THE FIRST LEG IS THE EXPENSIVE ONE AND IT IS DELIBERATE. Both wave bosses spawn
// at Market Row, so that is where the party stands when Salramm dies — and Arthas
// is 265yd west of it, beyond the 170yd instance grid activation range that is the
// only thing keeping a DB-spawned escort NPC ticking. The party has to go west and
// meet him. tp-20260909-224741-1 is what waiting at the door looks like: ten runs
// stood there for a counter only he could advance.
TEST(CullingOfStratholmeRouteProbe, TheTownHallAndMarketLegsRoute)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::vector<Pt> const from = { MARKET_ROW, TH_MEET,   TOWN_HALL, PASSAGE };
    std::vector<Pt> const to   = { TH_MEET,    TOWN_HALL, PASSAGE,   MARKET  };

    for (std::size_t i = 0; i < from.size(); ++i)
    {
        LegResult const leg = PrintLeg(mesh.get(), from[i], to[i]);
        EXPECT_TRUE(leg.route.reachable)
            << from[i].name << " -> " << to[i].name << " does not route: "
            << leg.route.failureReason;
        EXPECT_TRUE(leg.route.corridorComplete)
            << from[i].name << " -> " << to[i].name
            << " routes but stops short of the target poly";
        ExpectClearOfShutGates(leg, from[i].name);
    }
}

// THE TWO MEASUREMENTS THAT PIN THE TOWN HALL MEETING POINT, and the reason this
// leg has now failed live twice with the anchor at each end of Arthas's run.
//
// The meeting point has to satisfy both of these AT ONCE, and they pull in
// opposite directions along the road:
//
//   WAKE HIM. Arthas is a DB spawn and is only updated while a player is inside
//   the 170yd instance grid activation range. If the anchor is further than that
//   from his WP11 wave stop he never runs the 10s timer that unpauses him after
//   Salramm, never walks to WP20, and DATA_ARTHAS_EVENT never leaves KILLED_SALRAMM
//   (tp-20260909-224741-1, 0/10, anchored at the door).
//
//   THEN STILL BE ABLE TO SEE HIM. He does not wait to be collected: the timer
//   fires the moment the approaching party wakes him, and he runs the 275yd to the
//   Town Hall while they are still walking. So the whole of his WP12..WP20 run has
//   to lie inside the leg's escort search radius, or the party arrives at an empty
//   anchor and holds there (tp-20260910-073710-1, 0/10, anchored at WP12).
//
// Straight-line distance is the right metric for both: grid activation and
// FindNearestCreature are both range checks, not path lengths.
TEST(CullingOfStratholmeRouteProbe, TheTownHallMeetingPointWakesArthasAndKeepsHimInSearch)
{
    auto flatDist = [](Pt const& a, Pt const& b)
    {
        float const dx = a.x - b.x;
        float const dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    float const toStop = flatDist(TH_MEET, ARTHAS_STOP);
    float const toDoor = flatDist(TH_MEET, TOWN_HALL);
    std::printf("  Town Hall meeting point: %.1fyd from Arthas's WP11 stop, "
                "%.1fyd from the WP20 door (search %.0fyd)\n",
                toStop, toDoor, ESCORT_SEARCH_TOWN_HALL);

    EXPECT_LT(toStop, 170.0f)
        << "the meeting point is " << toStop << "yd from Arthas's WP11 stop, past the"
           " 170yd instance grid activation range: arriving there would not wake him,"
           " so he would never leave the wave stop and the counter would never move";

    EXPECT_LT(toDoor, ESCORT_SEARCH_TOWN_HALL)
        << "the Town Hall door is " << toDoor << "yd from the meeting point, outside"
           " the leg's " << ESCORT_SEARCH_TOWN_HALL << "yd escort search: a party that"
           " dawdles on the way (loot, rests, stragglers) would arrive after Arthas"
           " has already run the whole way and would never find him";

    // The far end of his run is the door, so covering it covers every point on it.
    // Assert that explicitly rather than trusting the road to stay monotonic.
    EXPECT_LT(flatDist(TH_MEET, ARTHAS_STOP), ESCORT_SEARCH_TOWN_HALL)
        << "the near end of Arthas's run is outside the escort search radius";

    // And it must not drift onto his stop: standing on top of him is the WP12
    // authoring, which is the one that let him run out from under the party.
    EXPECT_GT(toStop, 100.0f)
        << "the meeting point has drifted back toward Arthas's wave stop (" << toStop
        << "yd). The point is to meet him ON the road with room to see him coming,"
           " not to stand where he starts";
}

// THE MEASUREMENT THAT DECIDES WHERE THE HEROIC BONUS GOES, and the one this file
// exists to keep honest.
//
// The Infinite Corruptor is 81 yards from Market Row in a straight line, which is
// what makes it look as though the party could detour to him the moment Salramm
// dies. On foot he is 927 yards from it: the Market district he stands in is a
// separate component of this city's street graph, and the only way in is the one
// the dungeon intends — the Town Hall, the secret passage, Fire Street.
//
// So his objective sits at the ONE boundary it can: after the Fire Street leg,
// where Arthas stops at waypoint 54 with his gossip up and waits indefinitely, 143
// yards away. This pins both halves of that — the short leg and the long ones —
// because an mmaps regen or a re-authored anchor that quietly made one of the early
// legs "short" would be a 900-yard round trip nobody measured.
TEST(CullingOfStratholmeRouteProbe, TheCorruptorIsOnlyNearArthasPreMalganisStop)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    // The short one: the leg the objective actually walks, out and back.
    LegResult const out = PrintLeg(mesh.get(), MARKET, CORRUPTOR);
    LegResult const back = PrintLeg(mesh.get(), CORRUPTOR, MARKET);
    for (LegResult const* leg : { &out, &back })
    {
        EXPECT_TRUE(leg->route.reachable)
            << "the Corruptor detour does not route: " << leg->route.failureReason;
        EXPECT_TRUE(leg->route.corridorComplete);
        EXPECT_LT(leg->route.routeLength2d, 250.0f)
            << "the detour from Arthas's waypoint-54 stop is " << leg->route.routeLength2d
            << "yd. It was 143 when the objective was placed there; a leg much longer"
               " than that means the anchor or the mesh has moved and the heroic bonus"
               " should be re-sited.";
        ExpectClearOfShutGates(*leg, "the Corruptor detour");
    }

    // And the long ones, each of which is a place the objective must NOT be put.
    constexpr Pt kFarFrom[] = { MARKET_ROW, KS, ES };
    for (Pt const& f : kFarFrom)
    {
        LegResult const leg = PrintLeg(mesh.get(), f, CORRUPTOR);
        EXPECT_TRUE(leg.route.reachable) << f.name << " -> the Corruptor does not route";
        EXPECT_GT(leg.route.routeLength2d, 600.0f)
            << f.name << " is " << leg.route.routeLength2d
            << "yd from the Corruptor on foot. If the city has become connected this"
               " way, the heroic objective could move earlier in the order and win the"
               " 26-minute clock far more often — but check the gates first, because"
               " the navmesh cannot see them.";
    }
}

// The bookcase is the one door on the critical path, and it needs BOTH registry
// rows. Needs no navmesh, so it always runs.
TEST(CullingOfStratholmeRouteProbe, TheBookcaseIsScriptOnlyAndNavigationIgnored)
{
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(GO_SHKAF_GATE))
        << "GO 188686 (the Town Hall bookcase) must be script-only: Arthas opens it at"
           " his waypoint 36 and a bot Use() would both desync the client and race the"
           " escort.";
    EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(GO_SHKAF_GATE))
        << "GO 188686 must ALSO be navigation-ignored. IsScriptOnly only stops the"
           " click; the blocking-door value still flags it and the auto-pause under"
           " that is what ends the run — in front of a door that opens on its own"
           " seconds later, with the paused run unable to drive the event that opens"
           " it. The Halls of Reflection Frostmourne lesson.";

    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(GO_EXIT_GATE))
        << "GO 191788 (the City Entrance Gate) must be script-only: it opens only when"
           " the counter reaches FINISHED.";

    // And the mirror: the four shut city gates must NOT be whitelisted anywhere.
    // They are real obstacles — the routes above are asserted to avoid them — and
    // a door row would turn a wall into a silent shortcut or a silent stall.
    for (Gate const& g : kShutGates)
    {
        EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(g.entry))
            << g.name << " (" << g.entry << ") must not be navigation-ignored: it is a"
               " permanently shut gate that really does block, and the authored legs"
               " route around it.";
        EXPECT_FALSE(DcEventDoorRegistry::IsLockFreeClickable(g.entry))
            << g.name << " (" << g.entry << ") must not be lock-free-clickable: opening"
               " it would let the party shortcut a street the dungeon closes on"
               " purpose, and nothing in the core ever opens it.";
    }
}

