/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Certification probe for the hand-authored Halls of Reflection (map 668)
// geometry: the altar camp, the three objective anchors, the areatrigger forge
// point, and the four escape stand points.
//
// WHAT THIS IS A GATE ON, and why it is not a nicety.
//
// This dungeon has more authored POINTS than authored route, and every one of
// them carries an invariant that is invisible from the source:
//
//   1. THE CAMP. The party holds it for six to nine minutes under a leash that
//      wipes the whole wave event — and respawns every dead mob — if anybody
//      strays more than 70.5yd from CenterPos. It has to be on the mesh, it has
//      to be inside the 40yd radius the automatic restart waits for, and it has
//      to leave enough room that a 4s fear plus a knockback cannot reach the
//      wipe line.
//
//   2. THE FOUR STAND POINTS. Each must be AHEAD of the leader's stop along the
//      bearing to that wall — that is the entire mechanism by which the party
//      stays out of a 10yd 7068-per-second ring and out of a zap rule whose
//      knockback makes itself worse. A stand point that snapped BEHIND its stop
//      would put the party in exactly the place the driver exists to keep them
//      out of, and nothing in a live run would name it.
//
//   3. THE FORGE POINT. Areatrigger 5605 is a BOX, and the core re-tests
//      containment on the packet — so a forge from outside it is a silent no-op
//      and the run stalls in the throne room with nothing to say.
//
//   4. THE OBJECTIVE ANCHORS. The General's must be short of his 30yd evade
//      radius (dragging him out of it despawns his reflections and costs the
//      run); the throne anchor must be OUTSIDE the areatrigger box, because
//      arriving there must not be the same act as starting the cutscene.
//
// The suite also PRINTS the routed polylines. That is deliberate: this is the
// tool the anchors were authored with (route the leg, decimate the polyline —
// [[dc-navharness-prints-the-route]]), so an mmaps regen that moves a corridor is
// re-authored the same way instead of by hand.
//
// Not a committed regression: reads the FULL (unsliced) mmaps dir from env
// DC_PROBE_MMAPS and GTEST_SKIPs when unset, same contract as the Azjol-Nerub,
// Blackwing Lair, Halls of Lightning, Mechanar, Ramparts, Utgarde Pinnacle and
// Pit of Saron probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='HallsOfReflectionRouteProbe.*'

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
    using namespace DcHallsOfReflection;

    struct Pt { float x, y, z; char const* name; };

    // The entrance is DcTestDungeonRegistry's map-668 row — the
    // areatrigger_teleport target a walked-in party lands on. Falric and Marwyn
    // are their live `creature` rows (both derive correctly on this map; they are
    // here so the legs can be routed end to end).
    constexpr Pt ENTRANCE = { 5239.01f, 1932.64f, 707.70f, "entrance"           };
    constexpr Pt INTRO    = { INTRO_X,  INTRO_Y,  INTRO_Z,  "OBJ(1) intro"      };
    constexpr Pt CAMP     = { CAMP_X,   CAMP_Y,   CAMP_Z,   "the altar camp"    };
    constexpr Pt CENTER   = { CENTER_X, CENTER_Y, CENTER_Z, "CenterPos"         };
    constexpr Pt FALRIC   = { 5284.16f, 2030.69f, 709.32f,  "Falric"            };
    constexpr Pt MARWYN   = { 5335.33f, 1982.38f, 709.32f,  "Marwyn"            };
    constexpr Pt GENERAL  = { GENERAL_X, GENERAL_Y, GENERAL_Z, "OBJ(2) General" };
    constexpr Pt GEN_HOME = { GENERAL_HOME_X, GENERAL_HOME_Y, GENERAL_HOME_Z,
                              "the General's home" };
    constexpr Pt THRONE   = { THRONE_X, THRONE_Y, THRONE_Z, "OBJ(3) throne"     };
    constexpr Pt FORGE    = { FORGE_X,  FORGE_Y,  FORGE_Z,  "the 5605 forge point" };
    constexpr Pt MUSTER   = { MUSTER_X, MUSTER_Y, MUSTER_Z, "the escape muster" };

    // Snap box for the on-mesh assertion. Horizontal stays tight so a miss means
    // "this point is not standing anywhere near here" rather than "something was
    // found across the chamber"; vertical covers the ordinary float between an
    // authored z and the mesh surface under it.
    constexpr float SNAP_H = 4.0f;
    constexpr float SNAP_V = 6.0f;

    // How far the mesh may move an authored point before it stops being an
    // authored point. A couple of yards is ordinary detail-mesh float; more than
    // that and it was written somewhere the party cannot stand.
    constexpr float SNAP_TOLERANCE = 3.0f;

    // Largest vertical step between consecutive corridor points that is still a
    // WALK. The escape path climbs 51yd over 679, all of it ramp.
    constexpr float MAX_STEP_Z = 6.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkipReason(std::string& why)
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
        {
            why = "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 668";
            return nullptr;
        }
        std::shared_ptr<dtNavMesh> mesh = DcNavHarness::LoadMap(dir, MAP_ID);
        if (!mesh)
            why = std::string("no map-668 navmesh under ") + dir + "/mmaps";
        return mesh;
    }

    std::vector<WaypointHint> const* GeneralRoute()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL,
                                              BossRosterRegistry::ObjectiveEntry(2));
    }

    std::vector<WaypointHint> const* ThroneRoute()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL,
                                              BossRosterRegistry::ObjectiveEntry(3));
    }

    std::vector<WaypointHint> const* EscapeRoute()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL,
                                              NPC_LICH_KING);
    }

    // Route a leg and print its polyline. The printing IS the authoring tool.
    DcNavHarness::RouteResult PrintLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh, MAP_ID, a.x, a.y, a.z, b.x, b.y, b.z);
        std::printf("\n=== Halls of Reflection (668): %s -> %s ===\n", a.name, b.name);
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

    float Dist2(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx, dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    void SnapCheck(dtNavMesh const* mesh, char const* what, float x, float y, float z)
    {
        G3D::Vector3 snapped;
        bool const ok = DcNavHarness::NearestPoint(mesh, x, y, z, SNAP_H, SNAP_V, snapped);
        float const d = ok ? Dist3(x, y, z, snapped.x, snapped.y, snapped.z) : -1.0f;

        std::printf("  [%-24s] (%8.2f, %9.2f, %7.2f)  ->  ", what, x, y, z);
        if (ok)
            std::printf("(%8.2f, %9.2f, %7.2f)  d=%.2f\n", snapped.x, snapped.y, snapped.z, d);
        else
            std::printf("OFF MESH\n");

        EXPECT_TRUE(ok) << what << " at (" << x << ", " << y << ", " << z
                        << ") is off the navmesh — a bot cannot stand there, so it can "
                           "neither walk to it nor forge an areatrigger from it";
        if (ok)
            EXPECT_LT(d, SNAP_TOLERANCE)
                << what << " snapped " << d << "yd — it is not standing where it was authored";
    }

    // Player::IsInAreaTriggerRadius's BOX branch, verbatim in shape: rotate the
    // player's offset by (2pi - box_orientation) about the trigger's centre, then
    // compare each axis independently against its half-extent.
    bool InsideThroneBox(float x, float y, float z)
    {
        double const rotation = 2.0 * 3.14159265358979323846 - AT_THRONE_YAW;
        double const s = std::sin(rotation);
        double const c = std::cos(rotation);

        double const dxRaw = x - AT_THRONE_X;
        double const dyRaw = y - AT_THRONE_Y;
        double const dx = dxRaw * c - dyRaw * s;
        double const dy = dyRaw * c + dxRaw * s;
        double const dz = z - AT_THRONE_Z;

        return std::fabs(dx) <= AT_THRONE_LENGTH / 2.0 &&
               std::fabs(dy) <= AT_THRONE_WIDTH / 2.0 &&
               std::fabs(dz) <= AT_THRONE_HEIGHT / 2.0;
    }
}

// --- the invariants that need no navmesh ------------------------------------

// THE ONE THAT DECIDES THE ESCAPE. Each stand point must be AHEAD of the leader's
// stop along the bearing to that wall — which, because the path runs -x AND -y
// throughout, is the same as saying its (x + y) is BELOW the stop's.
//
// That single scalar is the encounter's own rule: npc_hor_lich_kingAI zaps every
// player whose (p.x - lk.x) + (p.y - lk.y) exceeds 20 for 10 000 damage plus a
// knockback that throws them further behind. Standing the party ahead of the
// leader is how the module keeps every member on the safe side of it, so a stand
// point that drifted behind its stop would defeat the whole design silently.
TEST(HallsOfReflectionRouteProbe, EveryStandPointIsAheadOfItsStopAndShortOfItsWall)
{
    for (uint8 k = 0; k < 4; ++k)
    {
        HorPoint const& stop = PATH_WAYPOINTS[WP_STOP[k + 1]];
        HorPoint const& wall = ICE_WALL_TARGETS[k];
        HorPoint const& stand = STAND_POINTS[k];

        float const stopToWall = Dist2(stop.x, stop.y, wall.x, wall.y);
        float const stopToStand = Dist2(stop.x, stop.y, stand.x, stand.y);
        float const standToWall = Dist2(stand.x, stand.y, wall.x, wall.y);

        std::printf("  wall %u: stop->wall %.1fyd, stop->stand %.1fyd, stand->wall %.1fyd, "
                    "sum(stand)-sum(stop) %+.2f\n",
                    k + 1, stopToWall, stopToStand, standToWall,
                    (stand.x + stand.y) - (stop.x + stop.y));

        // AHEAD, on the encounter's own scalar.
        EXPECT_LT(stand.x + stand.y, stop.x + stop.y)
            << "stand point " << (k + 1) << " is BEHIND the leader's stop on the (x + y) "
               "scalar the Lich King's zap rule is measured with — the party would hold "
               "ground on the wrong side of the leader";

        // Roughly the authored offset, and never past the wall. The leader stops
        // 24-25yd short of each wall and the wall blocks further movement, so a
        // stand point past it is a point the party cannot reach.
        EXPECT_NEAR(stopToStand, STAND_AHEAD_YD, 1.0f)
            << "stand point " << (k + 1) << " is not " << STAND_AHEAD_YD
            << "yd ahead of its stop";
        EXPECT_LT(standToWall, stopToWall)
            << "stand point " << (k + 1) << " is not between the leader and her wall";
        EXPECT_GT(standToWall, 10.0f)
            << "stand point " << (k + 1) << " is inside the ice wall itself";
    }
}

// The camp is the ground the party holds for the entire first half. Three
// numbers, all from the instance script itself.
TEST(HallsOfReflectionRouteProbe, TheAltarCampSitsInsideBothLeashesWithRoomToBeFeared)
{
    float const toCenter = Dist2(CAMP_X, CAMP_Y, CENTER_X, CENTER_Y);
    std::printf("  camp -> CenterPos: %.1fyd (restart radius %.0f, wipe line %.1f)\n",
                toCenter, LEASH_RESTART, LEASH_COMBAT);

    // INSIDE THE RESTART RADIUS. After a leash wipe the instance re-arms the whole
    // event on its own, but only once every non-GM player is alive AND within 40yd
    // of CenterPos — so the driver's RESTART state works by walking the party to
    // the camp, and that only works if the camp is inside it.
    EXPECT_LT(toCenter, LEASH_RESTART)
        << "the camp is outside the instance's own restart radius, so holding it would "
           "never satisfy the automatic wave restart";

    // AND FAR ENOUGH INSIDE THE WIPE LINE. Falric's Defiling Horror is a 4-second
    // AoE fear (~28yd of travel) and the Ghostly Priests' Circle of Destruction is
    // a 10yd knockback; both can happen to the same bot. 30yd of margin is the
    // floor.
    EXPECT_GT(LEASH_COMBAT - toCenter, 30.0f)
        << "the camp leaves less than 30yd of margin to the 70.5yd wipe line — a feared "
           "bot would take the whole wave event down with it";

    // NOT ON THE ALTAR. The leader, Uther and the intro Lich King stand there for
    // the whole cutscene, and Marwyn's Well of Corruption needs floor to step out
    // onto.
    EXPECT_GT(toCenter, 10.0f)
        << "the camp is on top of the altar the intro's RP actors occupy";
}

// The forge point must be inside areatrigger 5605's box and the objective anchor
// must be outside it. Arriving at the objective must not be the same act as
// starting the cutscene, because the step after the cutscene is the point of no
// return.
TEST(HallsOfReflectionRouteProbe, TheForgePointIsInsideTheThroneBoxAndTheAnchorIsOutside)
{
    EXPECT_TRUE(InsideThroneBox(FORGE_X, FORGE_Y, FORGE_Z))
        << "the forge point is outside areatrigger " << AREATRIGGER_THRONE
        << "'s box — HandleAreaTriggerOpcode re-tests containment, so the forge would be "
           "a SILENT no-op and the run would stall in the throne room";

    EXPECT_FALSE(InsideThroneBox(THRONE_X, THRONE_Y, THRONE_Z))
        << "the OBJ(3) anchor is inside areatrigger " << AREATRIGGER_THRONE
        << "'s box, so simply arriving at the objective would start the freeze cutscene "
           "before the party has gathered";

    // ...and with room for the arrival leash: a leader parked FORGE_LEASH off the
    // point must still be inside.
    for (float const dx : { -FORGE_LEASH, FORGE_LEASH })
        for (float const dy : { -FORGE_LEASH, FORGE_LEASH })
            EXPECT_TRUE(InsideThroneBox(FORGE_X + dx, FORGE_Y + dy, FORGE_Z))
                << "the forge point has less than its own arrival leash of margin inside "
                   "the trigger box";
}

// The General's anchor has to be short of the 30yd evade radius he is dragged out
// of, and the muster point has to be beside the leader rather than on the frozen
// Lich King.
TEST(HallsOfReflectionRouteProbe, TheAnchorsRespectTheirEncountersOwnRadii)
{
    float const toGeneral = Dist2(GENERAL_X, GENERAL_Y, GENERAL_HOME_X, GENERAL_HOME_Y);
    std::printf("  OBJ(2) -> the General's home: %.1fyd (his evade radius is %.0f)\n",
                toGeneral, GENERAL_EVADE_RADIUS);
    EXPECT_LT(toGeneral, GENERAL_EVADE_RADIUS)
        << "the General's objective anchor is outside his own 30yd evade radius, so the "
           "party would form up on ground he cannot be fought on";
    EXPECT_GT(toGeneral, 15.0f)
        << "the General's objective anchor is close enough that arriving IS the pull";

    float const toLeader = Dist2(MUSTER_X, MUSTER_Y, LEADER_ESCAPE_X, LEADER_ESCAPE_Y);
    float const toLk = Dist2(MUSTER_X, MUSTER_Y, LK_SPAWN_X, LK_SPAWN_Y);
    std::printf("  muster -> the leader: %.1fyd, muster -> the frozen Lich King: %.1fyd\n",
                toLeader, toLk);
    EXPECT_LT(toLeader, 6.0f)
        << "the muster point is out of gossip reach of the leader";
    EXPECT_GT(toLk, LK_RING_KEEPOUT)
        << "the muster point is inside the Lich King's Remorseless Winter ring — the party "
           "would start the escape already taking 7000 frost a second";
}

// The authored rows exist, cover where the party will be standing, and the escape
// row carries NO_STOP on every leg but its last.
TEST(HallsOfReflectionRouteProbe, TheAuthoredRowsAreRegisteredAndTheEscapeIsAllNoStop)
{
    std::vector<WaypointHint> const* general = GeneralRoute();
    std::vector<WaypointHint> const* throne = ThroneRoute();
    std::vector<WaypointHint> const* escape = EscapeRoute();

    ASSERT_NE(general, nullptr) << "the Marwyn -> Frostsworn General leg is not registered";
    ASSERT_NE(throne, nullptr) << "the General -> throne room leg is not registered";
    ASSERT_NE(escape, nullptr) << "the escape corridor is not registered";

    // SeedCursor projects the bot onto the row from where it stands, so a row that
    // starts somewhere else snaps the cursor to the far end
    // ([[dc-anchor-route-must-cover-where-the-party-stands]]).
    EXPECT_LT(Dist2(general->front().x, general->front().y, CENTER_X, CENTER_Y), 8.0f)
        << "the General leg does not start on the altar the party is standing on when "
           "Marwyn dies";
    EXPECT_LT(Dist2(throne->front().x, throne->front().y, GENERAL_X, GENERAL_Y), 8.0f)
        << "the throne leg does not start at the General's anchor";
    EXPECT_LT(Dist2(escape->front().x, escape->front().y, MUSTER_X, MUSTER_Y), 8.0f)
        << "the escape leg does not start at the muster point the gossip is taken from";

    EXPECT_LT(Dist2(escape->back().x, escape->back().y,
                    PATH_WAYPOINTS[18].x, PATH_WAYPOINTS[18].y), 2.0f)
        << "the escape leg does not end at WP18, which is the Lich King's roster anchor";

    // NO_STOP on every leg but the last. The flag covers the leg LEAVING an
    // anchor, so the final one carries nothing.
    for (std::size_t i = 0; i + 1 < escape->size(); ++i)
        EXPECT_TRUE(HasFlag((*escape)[i].flags, AnchorFlag::NO_STOP))
            << "escape anchor " << i << " is missing NO_STOP — a camp planned anywhere on "
               "this corridor is a camp the party is dragged BACKWARD to, toward a Lich "
               "King who is permanently ten to thirty yards behind them";

    // AND IT RUNS STRICTLY -x -y FROM WP0 ON, which is what makes a yield to
    // DcRel::Advance safe: whatever walks the party along this row walks them AWAY
    // from him.
    //
    // THE FIRST LEG IS EXEMPT AND HAS TO BE. The muster point is beside the leader
    // at LeaderEscapePos, and her path's own WP0 is 14yd EAST of it — the escape
    // starts by looping out of the throne room's mouth before it turns south-west,
    // so anchors 0 -> 1 genuinely move backward on the scalar. That is safe at
    // exactly the moment it happens and at no other: for the first ~16 seconds the
    // Lich King has no Remorseless Winter, and without it neither the zap rule nor
    // the ring exists. Every anchor after that is monotone, which is the window
    // the assertion is protecting.
    for (std::size_t i = 2; i < escape->size(); ++i)
    {
        float const prev = (*escape)[i - 1].x + (*escape)[i - 1].y;
        float const cur = (*escape)[i].x + (*escape)[i].y;
        EXPECT_LT(cur, prev)
            << "escape anchor " << i << " moves BACKWARD along the (x + y) scalar the "
               "Lich King's zap rule uses; the row must be monotone so an Advance fallback "
               "can only ever walk forward";
    }
}

// --- the navmesh half --------------------------------------------------------

TEST(HallsOfReflectionRouteProbe, EveryAuthoredPointIsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    std::printf("\n=== Halls of Reflection (668): authored points vs the mesh ===\n");
    SnapCheck(mesh.get(), "entrance", ENTRANCE.x, ENTRANCE.y, ENTRANCE.z);
    SnapCheck(mesh.get(), "OBJ(1) intro anchor", INTRO.x, INTRO.y, INTRO.z);
    SnapCheck(mesh.get(), "the altar camp", CAMP.x, CAMP.y, CAMP.z);
    SnapCheck(mesh.get(), "OBJ(2) General anchor", GENERAL.x, GENERAL.y, GENERAL.z);
    SnapCheck(mesh.get(), "OBJ(3) throne anchor", THRONE.x, THRONE.y, THRONE.z);
    SnapCheck(mesh.get(), "the 5605 forge point", FORGE.x, FORGE.y, FORGE.z);
    SnapCheck(mesh.get(), "the escape muster", MUSTER.x, MUSTER.y, MUSTER.z);

    for (uint8 k = 0; k < 4; ++k)
    {
        std::string const name = "escape stand " + std::to_string(k + 1);
        SnapCheck(mesh.get(), name.c_str(), STAND_POINTS[k].x, STAND_POINTS[k].y,
                  STAND_POINTS[k].z);
    }

    // The Lich King's roster anchor. A boss anchor that does not snap is DROPPED
    // AT LOAD with one LOG_ERROR ([[dc-boss-anchor-snap-vertical-extent]]), which
    // on this map would silently take the escape out of the roster.
    SnapCheck(mesh.get(), "the Lich King anchor (WP18)", PATH_WAYPOINTS[18].x,
              PATH_WAYPOINTS[18].y, PATH_WAYPOINTS[18].z);
}

TEST(HallsOfReflectionRouteProbe, EveryAuthoredAnchorIsOnTheMesh)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    struct Row { char const* name; std::vector<WaypointHint> const* hints; };
    Row const rows[] = {
        { "Marwyn -> the General", GeneralRoute() },
        { "the General -> the throne room", ThroneRoute() },
        { "the escape corridor", EscapeRoute() },
    };

    for (Row const& row : rows)
    {
        ASSERT_NE(row.hints, nullptr) << row.name << " is not registered";
        std::printf("\n=== Halls of Reflection (668): %s (%zu anchors) ===\n",
                    row.name, row.hints->size());
        for (std::size_t i = 0; i < row.hints->size(); ++i)
        {
            std::string const label = std::string("anchor ") + std::to_string(i);
            SnapCheck(mesh.get(), label.c_str(), (*row.hints)[i].x, (*row.hints)[i].y,
                      (*row.hints)[i].z);
        }
    }
}

// The dungeon end to end, one leg at a time, printed. This is the authoring tool.
TEST(HallsOfReflectionRouteProbe, TheDungeonIsOneContinuousWalk)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    struct Leg { Pt a, b; };
    Leg const legs[] = {
        { ENTRANCE, INTRO   },
        { INTRO,    CAMP    },
        { CAMP,     FALRIC  },
        { CAMP,     MARWYN  },
        { CENTER,   GENERAL },
        { GENERAL,  GEN_HOME },
        { GENERAL,  THRONE  },
        { THRONE,   FORGE   },
        { FORGE,    MUSTER  },
    };

    for (Leg const& leg : legs)
    {
        DcNavHarness::RouteResult const r = PrintLeg(mesh.get(), leg.a, leg.b);
        EXPECT_TRUE(r.reachable) << leg.a.name << " -> " << leg.b.name
                                 << " is not reachable: " << r.failureReason;
        EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
            << leg.a.name << " -> " << leg.b.name << " has a " << r.maxStepZ
            << "yd vertical step — that is a ledge, not a walk";
    }
}

// The escape corridor, stand point to stand point. Printed for the same reason,
// and asserted because these are the legs the driver actually splines: 100-176yd
// each, against a Lich King who never pauses.
TEST(HallsOfReflectionRouteProbe, TheEscapeCorridorWalksStandPointToStandPoint)
{
    std::string why;
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkipReason(why);
    if (!mesh)
        GTEST_SKIP() << why;

    Pt const stands[] = {
        MUSTER,
        { STAND_POINTS[0].x, STAND_POINTS[0].y, STAND_POINTS[0].z, "stand 1" },
        { STAND_POINTS[1].x, STAND_POINTS[1].y, STAND_POINTS[1].z, "stand 2" },
        { STAND_POINTS[2].x, STAND_POINTS[2].y, STAND_POINTS[2].z, "stand 3" },
        { STAND_POINTS[3].x, STAND_POINTS[3].y, STAND_POINTS[3].z, "stand 4" },
        { PATH_WAYPOINTS[18].x, PATH_WAYPOINTS[18].y, PATH_WAYPOINTS[18].z, "WP18" },
    };

    for (std::size_t i = 0; i + 1 < std::size(stands); ++i)
    {
        DcNavHarness::RouteResult const r = PrintLeg(mesh.get(), stands[i], stands[i + 1]);
        EXPECT_TRUE(r.reachable)
            << stands[i].name << " -> " << stands[i + 1].name
            << " is not reachable: " << r.failureReason
            << " — the driver splines exactly this leg, on a clock it cannot slip";
        EXPECT_LT(r.maxStepZ, MAX_STEP_Z)
            << stands[i].name << " -> " << stands[i + 1].name << " has a " << r.maxStepZ
            << "yd vertical step";
    }
}
