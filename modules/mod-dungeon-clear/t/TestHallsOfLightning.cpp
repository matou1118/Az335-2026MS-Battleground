/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcNoStopZone.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransitDecision.h"
#include "TestRun/DcTestDungeonRegistry.h"

// Halls of Lightning (map 602) — the authored-data lints for the dungeon whose
// roster is perfect and whose second leg has no driver.
//
// Suite name deliberately begins DungeonEvent so it is picked up by the
// `DungeonEvent*` filter that t/run_tests.sh and .github/workflows/tests.yml both
// use; a suite named HallsOfLightning* would build, pass locally, and never run in
// CI.
//
// Every number checked here is either read out of the core
// (src/server/scripts/Northrend/Ulduar/HallsOfLightning/*), read out of the live
// world DB, or derived from the real map-602 mmtiles by
// t/TestHallsOfLightningRouteProbe — which is where the coordinates come from and
// which is the suite that re-derives them after an mmaps regen. These tests exist
// so an edit that drops one of those properties fails at author time rather than
// silently costing a run, which on this map means a party that kills Bjarngrim and
// then stands in a slag pit for the rest of the clear.

using namespace DcHallsOfLightning;

namespace
{
    std::vector<WaypointHint> const* Route()
    {
        return DungeonClearRouteRegistry::Get(MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_VOLKHAN);
    }

    // The corridor predicate's pure half — InTransitCorridor without a Player.
    // Kept in step with HallsOfLightningEvents.cpp by
    // TheCorridorPredicateAndTheseBoxesAgree below, which is the only thing that
    // makes this a legitimate stand-in.
    bool InCorridor(float x, float y, float z)
    {
        bool const inPit = x >= PIT_BOX_MIN_X && x <= PIT_BOX_MAX_X &&
                           y >= PIT_BOX_MIN_Y && y <= PIT_BOX_MAX_Y &&
                           z >= PIT_BOX_MIN_Z && z <= PIT_BOX_MAX_Z;
        if (inPit)
            return true;
        return x >= CLIMB_BOX_MIN_X && x <= CLIMB_BOX_MAX_X &&
               y >= CLIMB_BOX_MIN_Y && y <= CLIMB_BOX_MAX_Y &&
               z >= CLIMB_BOX_MIN_Z && z <= CLIMB_BOX_MAX_Z;
    }

    float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Volkhan's leash, verbatim from boss_volkhan.cpp's EVENT_POSITION:
    //   if (me->GetDistance(1331.9f, -106.0f, 56.0f) > 95.0f) EnterEvadeMode();
    constexpr float VOLKHAN_LEASH_X = 1331.9f;
    constexpr float VOLKHAN_LEASH_Y = -106.0f;
    constexpr float VOLKHAN_LEASH_Z = 56.0f;
    constexpr float VOLKHAN_LEASH_RADIUS = 95.0f;
}

// --- the event row ---------------------------------------------------------

TEST(DungeonEventHallsOfLightningTest, TransitIsARepeatablePersistentCombatDriverThatOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_SLAG_FURNACE_TRANSIT);
    ASSERT_NE(ev, nullptr) << "Halls of Lightning (602) event " << EVENT_SLAG_FURNACE_TRANSIT
                           << " (cross the Slag Furnace) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the transit predicate must be bound";

    // The load-bearing flag. Fourteen Slags on a 20s respawn cover the route line,
    // so party engagement never drops and a rung that stands down on IsInCombat()
    // never runs at all.
    EXPECT_TRUE(ev->drivesInCombat) << "the transit must DrivesInCombat — this is the whole point";

    // The driver delivers the leader on its own long-range spline; without this the
    // at-objective hold cancels last tick's glide before the hook can see it.
    EXPECT_TRUE(ev->stepsOwnMovement) << "the transit must StepsOwnMovement";

    // Zero advanced pulls across the gauntlet: the pull's Idle branch answers
    // unplanned aggro by dragging a camp BACKWARD along the route, which in a pit
    // on a 20s respawn never finds clear ground — and which would haul Volkhan
    // himself out of his 95yd leash if it happened on the descent.
    EXPECT_TRUE(ev->ownsThePull) << "the transit must OwnsThePull";

    // The crossing is not a thing that completes once, and a "combat gap" on this
    // leg is one Slag dying.
    EXPECT_TRUE(ev->repeatable) << "the transit must be Repeatable (no completion latch)";
    EXPECT_TRUE(ev->persistent) << "the transit must be Persistent (combat gaps must not rewind it)";

    // No encounter is in progress here — this is the leg BETWEEN two of them.
    EXPECT_FALSE(ev->encounterActive);

    // Not Optional: every hold is watchdog-bounded from inside, so the step's own
    // timeout can only fire when a watchdog has itself failed, and skipping there
    // hands the leg back to a clear that provably cannot cross it.
    EXPECT_TRUE(ev->required) << "the transit must NOT be Optional";

    ASSERT_EQ(ev->steps.size(), 1u)
        << "the transit must be exactly one Custom step (the driver hook); a step list"
           " cannot express a per-tick walk-or-hold preference";
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_SLAG_FURNACE_TRANSIT);
    EXPECT_EQ(ev->steps[0].timeoutMs, TRANSIT_TIMEOUT_MS);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_SLAG_FURNACE_TRANSIT))
        << "hook " << HOOK_SLAG_FURNACE_TRANSIT << " (DriveSlagFurnaceTransit) must be registered";
}

// PanelBeforeBoss is NOT cosmetic: DcTargeting::HasPendingSummonEvent reads
// panelGatesBossEntry as "this boss must still be SUMMONED" and suppresses the
// dynamic pull within 80yd of him. A Repeatable event is never latched, so that
// suppression would be permanent and the party would arrive at Volkhan and never
// pull him. [[dc-panelbeforeboss-repeatable-permanent-hold]].
TEST(DungeonEventHallsOfLightningTest, TransitSortsAfterBjarngrimAndNeverGatesVolkhan)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_SLAG_FURNACE_TRANSIT);
    ASSERT_NE(ev, nullptr);

    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_BJARNGRIM);
    EXPECT_EQ(ev->panelGatesBossEntry, 0u)
        << "PanelBeforeBoss on a Repeatable event is a PERMANENT pull hold at that boss";
}

TEST(DungeonEventHallsOfLightningTest, HookIdIsUniqueAcrossEveryDungeon)
{
    EXPECT_EQ(HOOK_SLAG_FURNACE_TRANSIT, 24u);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_SLAG_FURNACE_TRANSIT));

    // Ids are one flat space. Spot-check the neighbours this row was numbered
    // against so a future dungeon cannot quietly re-use 24.
    for (uint32 taken : { DcVioletHold::HOOK_DRIVE_WAVE,
                          DcBlackwingLair::HOOK_RAZORGORE_ORB,
                          DcBlackwingLair::HOOK_SUPPRESSION_TRANSIT,
                          DcHallsOfStone::HOOK_TRIBUNAL,
                          DcHallsOfStone::HOOK_WAVE })
        EXPECT_NE(HOOK_SLAG_FURNACE_TRANSIT, taken);
}

// --- the route -------------------------------------------------------------

TEST(DungeonEventHallsOfLightningTest, TheRouteIsRegisteredAndItsTwoContractualAnchorsHold)
{
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr) << "no authored Bjarngrim -> Volkhan route on map 602";
    ASSERT_GT(route->size(), TRANSIT_END_ANCHOR_INDEX)
        << "the row is shorter than the slice the driver is handed";

    // The driver slices the row here, so if these indices stop naming these points
    // its cursor 0 (the gather gate, the arm, the pin) and its LAST anchor (the
    // completion test) silently mean somewhere else.
    WaypointHint const& stage = (*route)[TRANSIT_STAGE_ANCHOR_INDEX];
    EXPECT_NEAR(stage.x, TRANSIT_STAGE_X, 0.01f)
        << "TRANSIT_STAGE_ANCHOR_INDEX no longer names the top of the descent";
    EXPECT_NEAR(stage.y, TRANSIT_STAGE_Y, 0.01f);
    EXPECT_NEAR(stage.z, TRANSIT_STAGE_Z, 0.01f);

    WaypointHint const& end = (*route)[TRANSIT_END_ANCHOR_INDEX];
    EXPECT_NEAR(end.x, TRANSIT_END_X, 0.01f)
        << "TRANSIT_END_ANCHOR_INDEX no longer names the mid ledge";
    EXPECT_NEAR(end.y, TRANSIT_END_Y, 0.01f);
    EXPECT_NEAR(end.z, TRANSIT_END_Z, 0.01f);

    // The row must run all the way to Volkhan: the halves the transit does NOT own
    // are what the ordinary clear walks and projects its cursor on to.
    WaypointHint const& last = route->back();
    EXPECT_LT(Dist3(last.x, last.y, last.z, 1332.38f, -102.08f, 56.98f), 2.0f)
        << "the row must end at Volkhan, not at the transit's own end";
}

// A leg longer than this is a leg the party cannot stay together across, and a
// leg steeper than this is a wall rather than a ramp.
//
// THE STEEPNESS TEST IS A SLOPE, NOT A VERTICAL STEP, and that distinction is
// the whole content of it. This route is decimated to ~16yd anchors from a
// corridor sampled every 4yd, so an anchor pair spans four corridor points and
// its rise is four steps' worth: the climb on to Volkhan's gallery rises 8.7yd
// between anchors 20 and 21 while the CORRIDOR's largest single step on the
// whole leg is 2.54yd. A flat dz cap would read that ramp as a ledge, and would
// miss a genuine ledge hidden inside a long flat leg. The mmap generator's own
// walkable limit is 60 degrees (slope 1.73); 1.0 (45 degrees) sits comfortably
// under it and comfortably over this route's steepest leg (0.59, the climb out
// of the pit), so it catches a regen that has stitched a wall into the corridor
// without arguing with the ramps that are really there. The per-STEP ledge test
// belongs to the probe suite, which has the corridor itself.
TEST(DungeonEventHallsOfLightningTest, EveryLegIsARampNotAWallAndFitsThePackLeash)
{
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_GE(route->size(), 2u);

    constexpr float MAX_LEG_3D = 22.0f;
    constexpr float MAX_SLOPE = 1.0f;

    for (std::size_t i = 1; i < route->size(); ++i)
    {
        WaypointHint const& a = (*route)[i - 1];
        WaypointHint const& b = (*route)[i];
        float const leg = Dist3(a.x, a.y, a.z, b.x, b.y, b.z);
        EXPECT_GT(leg, 0.5f) << "leg " << (i - 1) << " -> " << i << " is a duplicate anchor";
        EXPECT_LT(leg, MAX_LEG_3D) << "leg " << (i - 1) << " -> " << i << " is " << leg
                                   << "yd — longer than the party can stay together across";

        float const horiz = std::hypot(b.x - a.x, b.y - a.y);
        ASSERT_GT(horiz, 0.1f) << "leg " << (i - 1) << " -> " << i << " is a pure vertical drop";
        float const slope = std::fabs(b.z - a.z) / horiz;
        EXPECT_LT(slope, MAX_SLOPE)
            << "leg " << (i - 1) << " -> " << i << " has slope " << slope
            << " — that is a wall, not a ramp";
    }
}

// The slice the driver is actually handed, built by the shared helper. This is
// the parameterised half of DcTransit::Route working: a MIDDLE slice, where
// Blackwing Lair takes a tail one.
TEST(DungeonEventHallsOfLightningTest, TheDriversSliceStartsAtStagingAndEndsAtTheMidLedge)
{
    DcTransit::RouteView const* view =
        DcTransit::Route(MAP_ID, NPC_VOLKHAN, TRANSIT_STAGE_ANCHOR_INDEX,
                         TRANSIT_END_ANCHOR_INDEX);
    ASSERT_NE(view, nullptr) << "the transit slice could not be built";

    ASSERT_EQ(view->hints.size(), TRANSIT_END_ANCHOR_INDEX - TRANSIT_STAGE_ANCHOR_INDEX + 1);
    EXPECT_EQ(view->anchors.size(), view->hints.size());

    EXPECT_NEAR(view->hints.front().x, TRANSIT_STAGE_X, 0.01f);
    EXPECT_NEAR(view->hints.front().y, TRANSIT_STAGE_Y, 0.01f);
    EXPECT_NEAR(view->hints.back().x, TRANSIT_END_X, 0.01f);
    EXPECT_NEAR(view->hints.back().y, TRANSIT_END_Y, 0.01f);

    // Cached and stable: the driver holds this pointer across ticks.
    EXPECT_EQ(view, DcTransit::Route(MAP_ID, NPC_VOLKHAN, TRANSIT_STAGE_ANCHOR_INDEX,
                                     TRANSIT_END_ANCHOR_INDEX));

    // And Blackwing Lair's own tail slice still resolves through the same helper.
    DcTransit::RouteView const* bwl =
        DcTransit::Route(DcBlackwingLair::MAP_ID, DcBlackwingLair::NPC_BROODLORD_LASHLAYER,
                         DcBlackwingLair::TRANSIT_STAGE_ANCHOR_INDEX, SIZE_MAX);
    ASSERT_NE(bwl, nullptr);
    EXPECT_NEAR(bwl->hints.front().x, DcBlackwingLair::TRANSIT_STAGE_X, 0.01f);
    EXPECT_NEAR(bwl->hints.back().x, DcBlackwingLair::TRANSIT_END_X, 0.01f);
}

// --- the corridor box ------------------------------------------------------

// The corridor is the transit's real activation gate — one bbox away from being
// the only thing between a rung on every bot's combat engine and the other three
// encounters of this dungeon.
TEST(DungeonEventHallsOfLightningTest, TheCorridorHoldsTheCrossingAndNothingElse)
{
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_GT(route->size(), TRANSIT_END_ANCHOR_INDEX);

    for (std::size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        bool const want = i >= TRANSIT_STAGE_ANCHOR_INDEX && i <= TRANSIT_END_ANCHOR_INDEX;
        EXPECT_EQ(InCorridor(h.x, h.y, h.z), want)
            << "anchor " << i << " at (" << h.x << ", " << h.y << ", " << h.z << ") is "
            << (want ? "OUTSIDE" : "INSIDE") << " the corridor and must not be";
    }

    // The switchback is the reason there are two boxes rather than one: the climb
    // out of the pit (x 1340.5) and the climb on to Volkhan's gallery (x 1348+)
    // are parallel ramps 7.5yd apart, overlapping in y and z for their whole lower
    // halves. A leader on the gallery ramp inside the corridor would have its
    // cursor projected back on to the pit ramp and be walked back down the hole.
    EXPECT_FALSE(InCorridor(1348.03f, -230.46f, 38.57f)) << "the foot of the gallery ramp";
    EXPECT_FALSE(InCorridor(1348.59f, -218.47f, 43.95f)) << "the middle of the gallery ramp";
    EXPECT_FALSE(InCorridor(1349.34f, -202.49f, 52.65f)) << "the head of the gallery ramp";

    // ...and the box must not swallow anything else on the map.
    EXPECT_FALSE(InCorridor(1262.0f, -26.9f, 33.51f))   << "Bjarngrim's spawn";
    EXPECT_FALSE(InCorridor(1395.09f, 36.64f, 50.04f))  << "Bjarngrim's east platform";
    EXPECT_FALSE(InCorridor(1331.47f, 259.62f, 53.40f)) << "the entrance";
    EXPECT_FALSE(InCorridor(1332.38f, -102.08f, 56.98f))<< "Volkhan";
    EXPECT_FALSE(InCorridor(1322.23f, -89.25f, 61.33f)) << "Volkhan's anvil";
    EXPECT_FALSE(InCorridor(1350.09f, -186.51f, 52.68f))<< "Volkhan's gallery floor";
    EXPECT_FALSE(InCorridor(1225.81f, -164.37f, 52.4f)) << "the Hall of the Watchers";
    EXPECT_FALSE(InCorridor(1081.99f, -261.81f, 61.29f))<< "Ionar";
    EXPECT_FALSE(InCorridor(1186.47f, 33.83f, 60.81f))  << "Loken";
    EXPECT_FALSE(InCorridor(1330.0f, 30.0f, -1.88f))    << "the mid-hall void's under-map sheet";
}

// Volkhan's gallery is 29yd directly above the pit floor, which is why the Z band
// is load-bearing rather than decorative.
TEST(DungeonEventHallsOfLightningTest, TheGalleryIsOnlySeparatedFromThePitByHeight)
{
    // Same column, 29yd apart.
    EXPECT_TRUE(InCorridor(1350.0f, -186.5f, 23.88f)) << "the pit floor under the gallery";
    EXPECT_FALSE(InCorridor(1350.0f, -186.5f, 52.68f)) << "the gallery floor above it";
    EXPECT_LT(PIT_BOX_MAX_Z, 52.0f)
        << "the corridor ceiling has risen into Volkhan's gallery";
    EXPECT_EQ(PIT_BOX_MAX_Z, CLIMB_BOX_MAX_Z) << "the two boxes must share one ceiling";
    EXPECT_EQ(PIT_BOX_MIN_Z, CLIMB_BOX_MIN_Z) << "the two boxes must share one floor";
}

// --- the NO_STOP span ------------------------------------------------------

// The flag covers the leg LEAVING an anchor, so the span runs 5..17 and RELEASES
// at 18 — where the four 3600s trash spawns on the mid ledge are worth a real
// pull. Asserted against real pit coordinates rather than against the flags
// alone, because the property that matters is "may the party camp HERE".
TEST(DungeonEventHallsOfLightningTest, NoStopCoversTheDescentThePitAndTheClimbOut)
{
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    ASSERT_GT(route->size(), TRANSIT_END_ANCHOR_INDEX);

    for (std::size_t i = 0; i < route->size(); ++i)
    {
        bool const want = i >= TRANSIT_STAGE_ANCHOR_INDEX && i < TRANSIT_END_ANCHOR_INDEX;
        EXPECT_EQ(HasFlag((*route)[i].flags, AnchorFlag::NO_STOP), want)
            << "anchor " << i << "'s NO_STOP flag is wrong — the span must be ["
            << TRANSIT_STAGE_ANCHOR_INDEX << ", " << (TRANSIT_END_ANCHOR_INDEX - 1) << "]";
    }

    // 6yd is the routes' own arrive radius, i.e. "standing on the leg".
    constexpr float R = 6.0f;

    // The gauntlet: every one of these is a place a camp must never be set up.
    EXPECT_TRUE(DcNoStopZone::CoversPoint(*route, 1327.91f, -78.21f, 29.66f, R)) << "the descent";
    EXPECT_TRUE(DcNoStopZone::CoversPoint(*route, 1334.96f, -125.69f, 23.88f, R)) << "the pit, north";
    EXPECT_TRUE(DcNoStopZone::CoversPoint(*route, 1339.65f, -157.34f, 23.88f, R)) << "the pit, mid";
    EXPECT_TRUE(DcNoStopZone::CoversPoint(*route, 1340.68f, -189.26f, 23.88f, R)) << "the pit, south";
    EXPECT_TRUE(DcNoStopZone::CoversPoint(*route, 1340.53f, -213.26f, 30.50f, R)) << "the climb out";

    // ...and everywhere the party IS allowed to set one up.
    EXPECT_FALSE(DcNoStopZone::CoversPoint(*route, 1262.0f, -26.9f, 33.51f, R))
        << "Bjarngrim's floor is ordinary ground";
    EXPECT_FALSE(DcNoStopZone::CoversPoint(*route, 1348.03f, -230.46f, 38.57f, R))
        << "the mid ledge is where the z38 trash is fought";
    EXPECT_FALSE(DcNoStopZone::CoversPoint(*route, 1350.09f, -186.51f, 52.68f, R))
        << "Volkhan's gallery is ordinary ground";
}

// --- why the descent in particular must be crossed, not camped -------------
//
// Volkhan's leash is `GetDistance(1331.9, -106, 56) > 95 -> EnterEvadeMode`, and
// the pit floor is INSIDE it. A camp dragged down the ramp therefore takes the
// boss into the Slags over a path he cannot walk, and evades him — a second,
// independent reason for NO_STOP that has nothing to do with the Slags at all.
TEST(DungeonEventHallsOfLightningTest, ThePitFloorIsInsideVolkhansLeashButTheGalleryClimbIsNot)
{
    float const pit = Dist3(1332.0f, -150.0f, 23.4f, VOLKHAN_LEASH_X, VOLKHAN_LEASH_Y,
                            VOLKHAN_LEASH_Z);
    EXPECT_LT(pit, VOLKHAN_LEASH_RADIUS)
        << "the pit floor is " << pit << "yd from Volkhan's leash centre — if this ever "
           "exceeds 95 the second argument for NO_STOP on the descent is gone";

    // The transit's own end is comfortably outside it, which is why the crossing
    // can finish there without ever having engaged him.
    float const ledge = Dist3(TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z, VOLKHAN_LEASH_X,
                              VOLKHAN_LEASH_Y, VOLKHAN_LEASH_Z);
    EXPECT_GT(ledge, VOLKHAN_LEASH_RADIUS)
        << "the mid ledge is inside Volkhan's leash — the crossing would end in his fight";
}

// --- the doors -------------------------------------------------------------

// Both progression doors are lockId 0, which BotCanOpenDoorLikePlayer reads as
// "any player can click this". Only instance_halls_of_lightning's DoorData opens
// them, on SetBossState(..., DONE).
TEST(DungeonEventHallsOfLightningTest, BothPassageDoorsAreScriptOnlyAndStillNavigationVisible)
{
    for (uint32 go : { 191325u, 191326u })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go))
            << "GO " << go << " must be script-only — a bot that force-opened it would "
               "walk the party past a live boss";
        EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << "GO " << go << " is a REAL gate the party is stopped by; the at-boss "
               "stand-down has to see it";
    }

    // The five permanently-open archways are not on the list: they spawn state 0
    // with template Data0 = 1 and nothing on this map ever shuts them, so they
    // never reach the closed-door predicate at all.
    for (uint32 go : { 191324u, 191327u, 191328u, 191415u, 191416u })
        EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(go))
            << "GO " << go << " spawns open and is never shut — listing it is noise";
}

// --- the slime moats -------------------------------------------------------

// The two NAV_SLIME channels are priced so the router prefers the central
// walkway. A fence is never a cage: the walkway itself and every authored anchor
// must be untaxed.
TEST(DungeonEventHallsOfLightningTest, TheSlimeMoatsArePricedAndTheWalkwayIsNot)
{
    EXPECT_TRUE(DcNavPenaltyRegistry::HasVolumes(MAP_ID));

    // Inside each channel, at the slime surface (probed z 20.18).
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(MAP_ID, 1284.0f, -140.0f, 20.18f), 1.0f)
        << "the west moat is untaxed";
    EXPECT_GT(DcNavPenaltyRegistry::PenaltyAt(MAP_ID, 1376.0f, -184.0f, 20.18f), 1.0f)
        << "the east moat is untaxed";

    // The walkway at its NARROWEST (x 1314..1346 at y -140, measured by the probe)
    // must be clean, or the rows have pinched the only way through.
    for (float x : { 1314.0f, 1322.0f, 1332.0f, 1342.0f, 1346.0f })
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(MAP_ID, x, -140.0f, 20.18f), 1.0f)
            << "the walkway is taxed at x " << x << " — the fence has become a cage";

    // Every authored anchor, at its own height.
    std::vector<WaypointHint> const* route = Route();
    ASSERT_NE(route, nullptr);
    for (std::size_t i = 0; i < route->size(); ++i)
    {
        WaypointHint const& h = (*route)[i];
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(MAP_ID, h.x, h.y, h.z), 1.0f)
            << "anchor " << i << " stands inside a penalty region";
    }

    // The z band sits BELOW the walkway floor (23.88) on purpose, so the rows can
    // only ever tax an edge whose midpoint is on the liquid.
    EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(MAP_ID, 1284.0f, -140.0f, 23.88f), 1.0f)
        << "the moat rows reach up to the walkway's own floor height";
}

// --- the deliberate absences ----------------------------------------------

// This is the first WotLK map in the module whose roster derives correctly with
// no help. The absence is load-bearing: the transit is a CONDITIONAL event and a
// patch here could only add order keys and objectives that nothing needs.
TEST(DungeonEventHallsOfLightningTest, ThereIsNoRosterPatchAndThereMustNotBe)
{
    EXPECT_FALSE(BossRosterRegistry::HasPatch(MAP_ID))
        << "map 602's four encounters are all creditType 0 against real spawns and the "
           "DBC order IS the travel order — a roster patch can only make that worse";
}

// The Slag is NOT a never-target row, and that is a decision rather than an
// oversight: 0.7 spawns/s against fourteen low-HP mobs does not meet the class-3
// arithmetic bar (Blackwing Lair's whelps are 5.3/s across 375yd). The problem is
// the SNARE and the missing driver, and the transit addresses those. If the
// transit alone turns out not to cross the pit, the row wants a NEW justification
// written into that table's doc comment — not an existing class stretched to fit.
TEST(DungeonEventHallsOfLightningTest, TheSlagIsNotANeverTargetRow)
{
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, NPC_SLAG))
        << "adding this row is a design change, not a tuning change — read the note in "
           "DcHallsOfLightning before doing it";
}

// --- the driver's own numbers ---------------------------------------------

// Both are tunable-looking constants that only mean anything relative to each
// other, and they were tunable into direct contradiction on Blackwing Lair: the
// gather gate asked for a ball tighter than the ring the pack rung parks the
// party on, so it could only ever end by watchdog.
// [[dc-party-gate-must-read-the-rungs-ring]].
TEST(DungeonEventHallsOfLightningTest, TheGatherRadiusIsSatisfiableInsideThePackLeash)
{
    EXPECT_LT(TRANSIT_GATHER_RADIUS, TRANSIT_PACK_LEASH)
        << "a gather radius at or past the pack leash asks for a formation nothing produces";
    // The staging point has to be reachable from the gather: a radius wider than
    // the skip distance would arm the gather at a point the leader is already past.
    EXPECT_LT(TRANSIT_GATHER_RADIUS, TRANSIT_STAGE_SKIP_DIST);

    // The elite hold has to be able to arm before the party is already inside the
    // fight, and must not reach further than the scan that feeds it.
    EXPECT_LT(TRANSIT_ELITE_HOLD_RADIUS, TRANSIT_SCAN);

    // The two Unbound Firestorms flank anchor 16 at (1300.0, -214.8) and
    // (1362.2, -215.2). If the hold radius ever stops reaching them from the
    // route, the driver walks the party straight through both of them.
    float const toWest = Dist3(1340.53f, -213.26f, 30.50f, 1300.0f, -214.8f, 23.3f);
    float const toEast = Dist3(1340.53f, -213.26f, 30.50f, 1362.2f, -215.2f, 23.3f);
    EXPECT_LT(std::min(toWest, toEast), TRANSIT_SCAN)
        << "the elite scan cannot see either Firestorm from the climb out";
}

// --- the harness -----------------------------------------------------------

TEST(DungeonEventHallsOfLightningTest, TheTestDungeonRowPointsAtMap602)
{
    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find("hol");
    ASSERT_NE(row, nullptr) << "the .dc test registry has no 'hol' row";
    EXPECT_EQ(row->mapId, MAP_ID);
}
