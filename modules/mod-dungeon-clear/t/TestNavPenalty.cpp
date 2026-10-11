/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRouteFilter.h"

// Pure tests for the hand-authored no-go volume table, plus the one decision the
// route filter makes off it (does the fence apply to this query at all). No
// navmesh / map data required, so these run in every build (unlike the Tier-2 nav
// geometry suite).

TEST(DcNavPenaltyRegistry, ReportsMapsWithVolumes)
{
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(229));   // Lower Blackrock Spire
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(556));   // Sethekk Halls
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(546));   // Underbog
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(543));   // Hellfire Ramparts
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(389));   // Ragefire Chasm
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(43));    // Wailing Caverns
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(658));   // Pit of Saron
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(0));     // no rows
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(230));   // BRD — no rows
    EXPECT_FALSE(DcNavPenaltyRegistry::HasVolumes(560));   // Old Hillsbrad — no rows
}

TEST(DcNavPenaltyRegistry, PenalizesTheSethekkBackDoorRamp)
{
    // A point partway up the narrow x≈45 shortcut ramp (door y151,z0 ->
    // platform y250,z27): squarely inside the box, so it must be taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(556, 45.0f, 200.0f, 14.0f), 1.0f);

    // The legitimate western approach arrives on the platform at y>=250 — north
    // of the box. Ikiss's own position must be untaxed so the final hop is free.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, 44.7f, 287.0f, 25.2f), 1.0f);

    // The lower lobby south of the door (y<150) is the normal pre-Syth floor —
    // untaxed so early routing is unchanged.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, 45.0f, 130.0f, 0.3f), 1.0f);

    // The west ramp the long way actually uses (~(-250, 210)) is far from the
    // box — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, -250.0f, 210.0f, 27.0f), 1.0f);
}

TEST(DcNavPenaltyRegistry, FencesTheSethekkFallThroughCorner)
{
    // The five measured arc vertices round off a room corner where the navmesh
    // stitches a sliver of floor over a drop. A point in the middle of the pocket
    // (centroid of the arc, on the z≈26.7 floor) must be taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(556, -211.69f, 297.73f, 26.7f), 1.0f);

    // The arc's bounding-box top corners sit OUTSIDE the arc (the curve pulls
    // away from them) — open floor that must stay untaxed, which a box couldn't
    // achieve.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, -233.0f, 326.0f, 26.7f), 1.0f);

    // Same XY as the pocket but well below the floor band → a level below this
    // corner is not this hazard, so it is untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(556, -211.69f, 297.73f, 5.0f), 1.0f);

    // Geometrically inside the pocket, but a different map → no region applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -211.69f, 297.73f, 26.7f), 1.0f);
}

TEST(DcNavPenaltyRegistry, FencesTheHellfireRampartsCorridorWall)
{
    // A point on the wall line's midpoint (≈(-1351.55, 1656.98) at floor z68) sits
    // squarely inside the strip laid along the wall, so it must be taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(543, -1351.55f, 1656.98f, 68.46f), 1.0f);

    // A few yards off the wall, into the corridor centre (offset ~5yd along the
    // strip's inboard perpendicular): clear of the footprint, so untaxed. The
    // re-cut moved only the strip's FAR side, so this side is unchanged.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(543, -1348.58f, 1652.96f, 68.46f), 1.0f);

    // Same XY as the wall midpoint but well below the Z band → a different level is
    // not this hazard, so it is untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(543, -1351.55f, 1656.98f, 50.0f), 1.0f);

    // Geometrically on the wall, but a different map → no region applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -1351.55f, 1656.98f, 68.46f), 1.0f);
}

TEST(DcNavPenaltyRegistry, RampartsStripLeavesNoWalkableFloorBehindIt)
{
    // Regression: the strip used to be the measured line inflated ±2yd, but that
    // line is a straight chord across a navmesh edge that BOWS away from it, so
    // the middle of the strip ran 2-3yd inboard of the real drop-off and marooned
    // ≈29 sq yd of ordinary room floor between the strip and the cliff — floor a
    // party can stand on, reachable only by crossing a hard-reject region.
    //
    // (-1349.00, 1662.00) is in that pocket: real navmesh floor at z 68.70,
    // measured 2.5yd on the far side of the chord. It must be INSIDE the strip
    // now — not because the party should never be there, but so that "behind the
    // strip" is over the drop everywhere and there is no pocket left to be cut
    // off in the first place.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(543, -1349.0f, 1662.0f, 68.70f), 1.0f);

    // ...and so is the deepest floor the pocket reached: (-1353.57, 1662.63) at
    // z 68.61, 5.75yd out, the furthest any walkable sample gets from the chord.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(543, -1353.57f, 1662.63f, 68.61f), 1.0f);

    // The far boundary is 10yd out — past any floor, over the drop — so a point
    // beyond THAT is off the mesh entirely and needs no fencing.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(543, -1358.48f, 1666.77f, 68.46f), 1.0f);

    // The zone-in point itself is well clear of the strip and stays untaxed —
    // an ordinary start must route normally.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(543, -1355.24f, 1641.12f, 68.25f), 1.0f);
}

TEST(DcNavPenaltyRegistry, IsInsideRegionAgreesWithPenaltyAt)
{
    // The named predicate both consumers ask "is the party standing in a fence?"
    // with. It must be exactly "PenaltyAt says taxed", on every kind of row.
    EXPECT_TRUE(DcNavPenaltyRegistry::IsInsideRegion(543, -1351.55f, 1656.98f, 68.46f));  // polygon
    EXPECT_TRUE(DcNavPenaltyRegistry::IsInsideRegion(229, -126.1f, -390.3f, 44.4f));      // box
    EXPECT_TRUE(DcNavPenaltyRegistry::IsInsideRegion(389, -271.13f, -20.04f, -57.4f));    // RFC wall

    EXPECT_FALSE(DcNavPenaltyRegistry::IsInsideRegion(543, -1355.24f, 1641.12f, 68.25f)); // clear floor
    EXPECT_FALSE(DcNavPenaltyRegistry::IsInsideRegion(543, -1351.55f, 1656.98f, 50.0f));  // wrong Z
    EXPECT_FALSE(DcNavPenaltyRegistry::IsInsideRegion(0, -1351.55f, 1656.98f, 68.46f));   // no rows
}

// A fence says where routes may GO. It must never cage a party that is already
// standing inside one — which happens for real: the Ramparts strip covers ordinary
// room floor a few yards from where players zone in, so "started the run on the
// wrong side of the invisible wall" is a routine start, and taxing the way out at
// 40x is what sends the tank round the far side of the room instead.
TEST(DcRouteFilterTest, FenceIsLiveForAnOrdinaryStart)
{
    // Map with rows, start on clear floor → the fence applies as before.
    DcRouteFilter const filter(543, -1355.24f, 1641.12f, 68.25f);
    EXPECT_TRUE(filter.IsFenceActive());
}

TEST(DcRouteFilterTest, FenceStandsDownWhenTheRouteStartsInsideIt)
{
    // Start inside the Ramparts strip → the fence is off for this query, so the
    // A* can cost the way out at face value and leave.
    DcRouteFilter const rampart(543, -1351.55f, 1656.98f, 68.46f);
    EXPECT_FALSE(rampart.IsFenceActive());

    // Same for a box row (LBRS chasm) and for the RFC funnel wall — the rule is a
    // property of the registry, not of one hand-authored spot.
    DcRouteFilter const lbrs(229, -126.1f, -390.3f, 44.4f);
    EXPECT_FALSE(lbrs.IsFenceActive());

    DcRouteFilter const rfc(389, -271.13f, -20.04f, -57.4f);
    EXPECT_FALSE(rfc.IsFenceActive());
}

TEST(DcRouteFilterTest, MapsWithoutRowsNeverArmTheFence)
{
    // No rows on the map → nothing to test per edge, whatever the start.
    DcRouteFilter const none(0, 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(none.IsFenceActive());
}

TEST(DcNavPenaltyRegistry, FencesTheRagefireChasmFunnelWall)
{
    // The four measured points the wall is drawn through. Each must be taxed —
    // including the two outer ends, which is why every leg is extended past its
    // endpoints instead of terminating exactly on them (a point sitting on the
    // polygon's boundary edge is ill-defined for the even-odd test).
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -283.38f, -37.05f, -58.46f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -277.23f, -22.82f, -58.18f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -265.03f, -17.26f, -56.65f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -238.73f, -22.41f, -58.18f), 1.0f);

    // The midpoint of each leg — the wall is continuous along its whole length,
    // not just at the authored corners.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -280.31f, -29.94f, -58.3f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -271.13f, -20.04f, -57.4f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(389, -251.88f, -19.84f, -57.4f), 1.0f);

    // Six yards off the wall on either side (measured along leg 3's perpendicular):
    // the strip is thin, so open floor either side of it stays untaxed. This is the
    // point of a polygon here — leg 3's bounding box alone would be ~28x6yd and
    // would swallow floor the party legitimately uses.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(389, -253.03f, -25.72f, -58.0f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(389, -250.73f, -13.95f, -58.0f), 1.0f);

    // Same, off leg 1 — and far enough from leg 2 that the bend's overlap does not
    // reach it either.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(389, -274.80f, -32.32f, -58.0f), 1.0f);

    // On the wall in XY but well outside the Z band → a different level is not
    // this wall, so it is untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(389, -271.13f, -20.04f, -20.0f), 1.0f);

    // The instance's own start position is nowhere near the wall.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(389, 3.81f, -14.82f, -17.84f), 1.0f);

    // Geometrically on the wall, but a different map → no region applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -271.13f, -20.04f, -57.4f), 1.0f);
}

TEST(DcNavPenaltyRegistry, FencesTheWailingCavernsShortcutWall)
{
    // Both stitched climbs onto the west plateau are taxed. The reported ramp
    // (the tank left the lower floor at (-76.78, -259.83) and went up here) ...
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -77.91f, -262.80f, -59.38f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -83.00f, -261.33f, -56.09f), 1.0f);
    // ... and the second face 12yd north, which a ramp-only fence would just
    // hand the route instead (30yd -> 50yd rather than 30yd -> 240yd).
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -89.87f, -250.93f, -56.85f), 1.0f);

    // The strip is continuous across its bends: a point on each leg's midline.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -90.50f, -249.50f, -58.0f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -84.50f, -257.00f, -58.0f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(43, -77.00f, -264.00f, -58.0f), 1.0f);

    // Both sides of the wall stay untaxed — this is what a box could not do.
    // The plateau the route has to reach the long way round, and the Druid of
    // the Fang standing on it:
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -97.70f, -266.90f, -56.40f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -96.50f, -262.30f, -55.40f), 1.0f);
    // The lower east floor, including where the climb was reported from:
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -74.30f, -260.00f, -63.20f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -76.78f, -259.83f, -64.64f), 1.0f);
    // And the -60.5 rock spine the legitimate way round actually crosses.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -87.40f, -247.80f, -60.50f), 1.0f);

    // On the wall in XY but far off the Z band → a different tier of the cavern
    // is not this wall.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -84.50f, -257.00f, -20.0f), 1.0f);

    // The two off-mesh drops the dungeon events own are nowhere near the wall,
    // so the fence cannot interfere with them.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -290.66f, -3.83f, -58.30f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(43, -55.89f, 44.32f, -29.01f), 1.0f);

    // Geometrically on the wall, but a different map → no region applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -84.50f, -257.00f, -58.0f), 1.0f);
}

TEST(DcNavPenaltyRegistry, PenalizesInsideTheLbrsShaft)
{
    // The midpoint of the observed shortcut climb
    //   [-127.33,-402.11,30.32] -> [-124.88,-378.42,58.40]
    // is ≈(-126.1,-390.3,44.4): squarely inside the box, so it must be taxed.
    float const p = DcNavPenaltyRegistry::PenaltyAt(229, -126.1f, -390.3f, 44.4f);
    EXPECT_GT(p, 1.0f);
}

TEST(DcNavPenaltyRegistry, PenalizesInsideTheLbrsLedgeHop)
{
    // The midpoint of the second (small) shortcut
    //   [-61.70,-382.77,48.88] <-> [-64.34,-378.49,54.70]
    // is ≈(-63.0,-380.6,51.8): inside box #2's mid-Z band, so it is taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, -63.0f, -380.6f, 51.8f), 1.0f);
    // The lower walkway end (z below the band) is the legit approach — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -61.7f, -382.77f, 48.88f), 1.0f);
    // The upper platform end (z above the band), reached by the proper route from
    // another direction — untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -64.34f, -378.49f, 54.7f), 1.0f);
}

TEST(DcNavPenaltyRegistry, PenalizesTheUnderbogShortcut)
{
    // Both observed shortcut endpoints, and their midpoint, fall inside the box
    // that spans the whole wide-open run — all taxed.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(546, 35.17f, -364.37f, 27.57f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(546, 66.6f, -357.99f, 33.77f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(546, 50.9f, -361.2f, 30.7f), 1.0f);
    // Well outside the box on X → untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(546, 90.0f, -361.0f, 30.0f), 1.0f);
    // Below the box's Z floor → the legit floor beneath the climb is untaxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(546, 50.0f, -361.0f, 15.0f), 1.0f);
    // Inside the box geometrically, but a different map → no volume applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, 50.9f, -361.2f, 30.7f), 1.0f);
}

TEST(DcNavPenaltyRegistry, DoesNotPenalizeOutsideTheBox)
{
    // Same X/Y as the shaft but down on the lower floor (below the mid-Z band):
    // a route that legitimately belongs at the bottom must not be taxed.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, -126.0f, -390.0f, 30.0f), 1.0f);
    // Far away on the same map.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, 200.0f, 200.0f, 44.0f), 1.0f);
    // Inside the box geometrically, but a different map → no volume applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, -126.1f, -390.3f, 44.4f), 1.0f);
}

TEST(DcNavPenaltyRegistry, FencesThePitOfSaronNorthBridge)
{
    // The one bad link out of the three that cross the chasm between Krick's
    // arena and the ambush ramp: (865.87,76.75,524.31) -> (869.69,75.11,527.40).
    // Its midpoint, and both of its ends, sit inside the box.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(658, 867.78f, 75.93f, 525.86f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(658, 865.87f, 76.75f, 524.31f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(658, 869.69f, 75.11f, 527.40f), 1.0f);
    // The far end of the shortcut, where it lands on the ramp proper.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(658, 872.80f, 74.13f, 528.87f), 1.0f);

    // THE RAMP'S REAL FOOT MUST STAY FREE — this is the crossing the party is
    // being steered onto, so taxing it would defeat the row entirely.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 861.93f, 52.93f, 517.07f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 871.13f, 58.27f, 522.07f), 1.0f);

    // Every Leg A anchor between Krick and the top of the ambush ramp is clear.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 846.59f, 84.66f, 511.53f), 1.0f);  // 3
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 850.31f, 73.25f, 519.56f), 1.0f);  // 4
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 855.75f, 58.23f, 517.33f), 1.0f);  // 5
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 861.03f, 47.09f, 516.74f), 1.0f);  // 6 gate 1
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 871.83f, 52.31f, 523.03f), 1.0f);  // 7
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 882.64f, 57.54f, 530.37f), 1.0f);  // 8

    // The nearest point of the corridor the party actually walks down to gate 1
    // — outside the box on all three axes, and the reason the Z floor is 520.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 860.00f, 69.40f, 518.47f), 1.0f);

    // Under the box in Z: the arena-to-gate-1 descent runs beneath the bridge.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 867.00f, 76.00f, 517.00f), 1.0f);
    // Krick's arena floor and the ARM staging point are far outside it.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 852.85f, 123.53f, 510.11f), 1.0f);
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(658, 836.65f, 115.08f, 509.81f), 1.0f);
    // Inside the box geometrically, but a different map -> no volume applies.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(0, 867.78f, 75.93f, 525.86f), 1.0f);
}

// ---- Blackrock Spire LBRS / UBRS wing regions (map 229) ----------------------
// Wing-tagged rows fence a wing off for every OTHER wing's run. Points are the
// measured spawns and the shared-hall walk from the 2026-09-28 poly-graph survey
// (see the 229 block in DcNavPenaltyRegistry.cpp).
namespace
{
    struct P { char const* what; float x, y, z; };

    P const kLbrsPoints[] = {
        { "Omokk",       -22.8f, -300.7f,  31.8f },
        { "Vosh'gajin", -121.2f, -482.2f,  24.7f },
        { "Voone",       -17.0f, -459.1f, -18.6f },
        { "Smolderweb", -135.5f, -565.8f,  10.2f },
        { "Urok pile",   -14.4f, -395.8f,  48.5f },
        { "Zigris",     -190.5f, -475.6f,  87.4f },
        { "Halycon",    -193.9f, -338.1f,  64.5f },
        { "Wyrmthalak",  -22.6f, -486.2f,  90.8f },
    };

    P const kUbrsPoints[] = {
        { "Emberseer",     144.4f, -258.0f,  96.4f },
        { "Father Flame",   76.0f, -334.7f,  91.5f },
        { "Altar",         144.4f, -280.9f,  91.5f },
        { "Rend",          159.3f, -443.6f, 122.1f },
        { "The Beast",     124.2f, -563.8f, 107.4f },
        { "Drakkisath",     36.5f, -286.0f, 111.0f },
        { "rune room 1",   125.4f, -340.5f,  70.9f },
        { "rune room 3",   124.8f, -298.0f,  70.9f },
        { "rune room 6",   228.8f, -301.5f,  76.9f },
        { "Emberseer In",  216.4f, -286.1f,  76.9f },
    };

    // The shared entry hall: portal -> Dragonspine Door. Never fenced, for
    // either wing — a UBRS run and its rez recovery walk it.
    P const kSharedPoints[] = {
        { "portal",          78.5f, -225.0f, 49.8f },
        { "corridor 1",      80.2f, -248.8f, 60.4f },
        { "corridor 2",      92.5f, -274.3f, 61.1f },
        { "corridor 3",      96.0f, -305.1f, 64.6f },
        { "corridor 4",      95.4f, -324.0f, 66.2f },
        { "hall pack N",     79.0f, -287.1f, 60.8f },
        { "hall pack S",     85.2f, -358.0f, 60.8f },
        { "LBRS stair foot", 60.4f, -323.5f, 55.2f },
        { "door stall",     109.1f, -320.4f, 65.5f },
        { "door approach",  118.7f, -320.5f, 68.9f },
        { "UBRS entrance",  105.0f, -320.0f, 65.5f },
    };
}

TEST(DcNavPenaltyRegistry, WingRowsApplyOnlyToTheOtherWingsRun)
{
    EXPECT_TRUE(DcNavPenaltyRegistry::RowActive(nullptr, ""));
    EXPECT_TRUE(DcNavPenaltyRegistry::RowActive(nullptr, "lbrs"));
    EXPECT_TRUE(DcNavPenaltyRegistry::RowActive("", "lbrs"));
    EXPECT_TRUE(DcNavPenaltyRegistry::RowActive("ubrs", "lbrs"));
    EXPECT_FALSE(DcNavPenaltyRegistry::RowActive("ubrs", "ubrs"));
    // No run wing -> tagged rows are off: a caller that knows no wing never
    // fences anyone into one.
    EXPECT_FALSE(DcNavPenaltyRegistry::RowActive("ubrs", ""));
}

TEST(DcNavPenaltyRegistry, LbrsRunIsFencedOutOfUbrsOnly)
{
    for (P const& p : kUbrsPoints)
        EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z, "lbrs"), 1.0f) << p.what;
    for (P const& p : kLbrsPoints)
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z, "lbrs"), 1.0f) << p.what;
}

TEST(DcNavPenaltyRegistry, UbrsRunIsFencedOutOfLbrsOnly)
{
    for (P const& p : kLbrsPoints)
        EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z, "ubrs"), 1.0f) << p.what;
    for (P const& p : kUbrsPoints)
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z, "ubrs"), 1.0f) << p.what;
}

TEST(DcNavPenaltyRegistry, SharedBrsHallIsNeverFenced)
{
    for (char const* wing : { "", "lbrs", "ubrs" })
        for (P const& p : kSharedPoints)
            EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z, wing), 1.0f)
                << p.what << " (run wing '" << wing << "')";
}

TEST(DcNavPenaltyRegistry, WingRowsAreOffWithoutARunWing)
{
    for (P const& p : kUbrsPoints)
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(229, p.x, p.y, p.z), 1.0f) << p.what;
    // The untagged LBRS shortcut rows are unaffected by the wing column.
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, -126.1f, -390.3f, 44.4f), 1.0f);
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(229, -126.1f, -390.3f, 44.4f, "lbrs"), 1.0f);
}

TEST(DcNavPenaltyRegistry, WingRegionAtNamesTheHalfThePartyStandsIn)
{
    for (P const& p : kLbrsPoints)
    {
        char const* w = DcNavPenaltyRegistry::WingRegionAt(229, p.x, p.y, p.z);
        ASSERT_NE(w, nullptr) << p.what;
        EXPECT_STREQ(w, "lbrs") << p.what;
    }
    for (P const& p : kUbrsPoints)
    {
        char const* w = DcNavPenaltyRegistry::WingRegionAt(229, p.x, p.y, p.z);
        ASSERT_NE(w, nullptr) << p.what;
        EXPECT_STREQ(w, "ubrs") << p.what;
    }
    for (P const& p : kSharedPoints)
        EXPECT_EQ(DcNavPenaltyRegistry::WingRegionAt(229, p.x, p.y, p.z), nullptr) << p.what;
    // Other maps have no wing regions.
    EXPECT_EQ(DcNavPenaltyRegistry::WingRegionAt(429, 44.45f, -154.82f, -2.71f), nullptr);
}

TEST(DcRouteFilterTest, WingFenceArmsForTheRunWingAndNeverCages)
{
    // An LBRS run leaving the portal: the UBRS rows are armed.
    DcRouteFilter const lbrsAtPortal(229, 78.5f, -225.0f, 49.8f, "lbrs");
    EXPECT_TRUE(lbrsAtPortal.IsFenceActive());
    // An LBRS party that somehow stands in UBRS still routes out.
    DcRouteFilter const lbrsInUbrs(229, 144.4f, -258.0f, 96.4f, "lbrs");
    EXPECT_FALSE(lbrsInUbrs.IsFenceActive());
    // A UBRS run standing in its own wing is not "inside a fence".
    DcRouteFilter const ubrsAtEmberseer(229, 144.4f, -258.0f, 96.4f, "ubrs");
    EXPECT_TRUE(ubrsAtEmberseer.IsFenceActive());
}
