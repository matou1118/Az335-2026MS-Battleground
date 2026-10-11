/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// The Oculus (map 578).
//
// Three halves:
//
//   1. THE DRIVER KERNEL (DcOculusDriver::Decide) — the party plan: when to
//      muster, when to lift off, when to land, when the air belongs to Eregos,
//      and when nobody can be reached on foot.
//
//   2. THE RIDER AND FLIGHT KERNELS (DcOculusRider::Decide, DcOculusFlight) — one
//      member's essence, mount, leg, landing gate and Eregos station.
//
//   3. THE AUTHORED DATA — the roster patch, the nine events, the site table and
//      the glue rows (stand-down, purge, hazard).
//
// THE MOST IMPORTANT ASSERTIONS are the two safety invariants: nobody lifts off
// while anyone is engaged (a drake spell on a ground mob kills its rider), and
// nobody dismounts unless the landing gate passed (a clientless bot does not fall;
// it stands in the air until its next move drops it into the basement).

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "InstanceScript.h"

#include "Ai/Dungeon/DungeonClear/Data/DcCombatPurgeRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusDriverDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcOculusFlightDecision.h"

using namespace DcOculus;
using DcOculusDriver::Phase;
using DcOculusDriver::State;
using DcOculusFlight::Vec;
using DriverAction = DcOculusDriver::Action;
using RiderAction = DcOculusRider::Action;

namespace
{
    constexpr uint32 NOW = 1'000'000;

    // --- the driver's baselines ---------------------------------------------------
    //
    // Drakos dead, the next row the central ring, the whole party of five on foot on
    // Drakos's ring, topped off, nobody fighting. Each test breaks one thing.
    DcOculusDriver::Inputs OnDrakosRing()
    {
        DcOculusDriver::Inputs in;
        in.drakosDone = true;
        in.dest = SITE_R2C;
        in.tankAlive = true;
        in.tankMounted = false;
        in.tankIsland = SITE_R1_GIVERS;
        in.partySize = 5;
        in.mounted = 0;
        in.partyReady = true;
        in.nowMs = NOW;
        return in;
    }

    // The same party, all five in the saddle, still over Drakos's ring.
    DcOculusDriver::Inputs MountedOnTheRing()
    {
        DcOculusDriver::Inputs in = OnDrakosRing();
        in.tankMounted = true;
        in.mounted = 5;
        in.musterSinceMs = NOW - 10'000;
        return in;
    }

    // --- the rider's baseline ------------------------------------------------------
    //
    // A follower in the saddle, flying the Fly phase's leg to the central ring.
    DcOculusRider::Inputs Flying()
    {
        DcOculusRider::Inputs in;
        in.phase = Phase::Fly;
        in.dest = SITE_R2C;
        in.planFresh = true;
        in.isTank = false;
        in.alive = true;
        in.onVehicle = true;
        in.onOcDrake = true;
        in.seatCanControl = true;
        in.nowMs = NOW;
        return in;
    }

    auto const kClear = [](Vec const&, Vec const&) { return true; };

    BossRosterPatch const* Patch()
    {
        for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
            if (p.mapId == MAP_ID)
                return &p;
        return nullptr;
    }

    DungeonBossInfo const* Added(BossRosterPatch const& p, uint32 entry)
    {
        for (DungeonBossInfo const& b : p.add)
            if (b.entry == entry)
                return &b;
        return nullptr;
    }

    DungeonEvent const* Ev(uint32 id)
    {
        for (DungeonEvent const& e : DungeonEventRegistry::AllEvents())
            if (e.mapId == MAP_ID && e.id == id)
                return &e;
        return nullptr;
    }
}

// ===========================================================================
//  1. THE DRIVER KERNEL
// ===========================================================================

TEST(Oculus, InstanceDataSlotsMatchTheCore)
{
    // oculus.h enum Data, hand-copied: DATA_DRAKOS .. DATA_EREGOS, MAX_ENCOUNTER, DATA_CC_COUNT.
    EXPECT_EQ(DATA_DRAKOS, 0u);
    EXPECT_EQ(DATA_VAROS, 1u);
    EXPECT_EQ(DATA_UROM, 2u);
    EXPECT_EQ(DATA_EREGOS, 3u);
    EXPECT_EQ(DATA_CC_COUNT, 5u);
    EXPECT_EQ(STATE_DONE, static_cast<uint32>(DONE));
    EXPECT_EQ(STATE_IN_PROGRESS, static_cast<uint32>(IN_PROGRESS));
    EXPECT_EQ(CC_TOTAL, 10u);
}

TEST(Oculus, DriverYieldsUntilDrakosDies)
{
    DcOculusDriver::Inputs in = OnDrakosRing();
    in.drakosDone = false;
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::NotDue);
    EXPECT_EQ(v.action, DriverAction::Yield);
    EXPECT_EQ(v.phase, Phase::Idle) << "the portal and Drakos are the ordinary clear's";
}

TEST(Oculus, DriverMustersOnFootWhenTheNextSiteIsAnotherIsland)
{
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(OnDrakosRing());
    EXPECT_EQ(v.state, State::Muster);
    EXPECT_EQ(v.action, DriverAction::Hold) << "Advance cannot path between islands; keep it off the tank";
    EXPECT_EQ(v.phase, Phase::Muster);
    EXPECT_EQ(v.dest, SITE_R2C);
    EXPECT_EQ(v.musterSinceMs, NOW);
}

TEST(Oculus, DriverRestsBeforeAFreshMount)
{
    DcOculusDriver::Inputs in = OnDrakosRing();
    in.partyReady = false;
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Resting);
    EXPECT_EQ(v.action, DriverAction::Yield);
    EXPECT_EQ(v.phase, Phase::Idle);
    EXPECT_EQ(v.musterSinceMs, 0u);
}

TEST(Oculus, DriverYieldsWhileAnyoneFightsOnFoot)
{
    DcOculusDriver::Inputs in = OnDrakosRing();
    in.anyEngaged = true;
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Fight);
    EXPECT_EQ(v.action, DriverAction::Yield);
    EXPECT_EQ(v.phase, Phase::Idle);
}

TEST(Oculus, NeverFliesWithAMemberInCombat)
{
    // The whole party is mounted over its start island, the quorum is long past —
    // and somebody is fighting. Lift-off waits.
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.anyEngaged = true;
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);
    EXPECT_NE(v.phase, Phase::Fly) << "a drake spell on a ground mob kills its rider";
    EXPECT_EQ(v.state, State::Fight);
    EXPECT_EQ(v.action, DriverAction::Hold) << "the ladder stays off a mounted tank";

    in.anyEngaged = false;
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Fly);
}

TEST(Oculus, MusterWaitsForFiveThenTimesOutAtFour)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.mounted = 4;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Muster) << "four of five, ten seconds in";
    EXPECT_FALSE(v.musterTimedOut);

    in.musterSinceMs = NOW - MUSTER_TIMEOUT_MS;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Fly) << "four of five at the timeout is enough";

    in.mounted = 3;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Muster) << "three is not";
    EXPECT_TRUE(v.musterTimedOut);

    in.mounted = 5;
    in.musterSinceMs = NOW - 1000;
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Fly) << "five lifts off at once";

    // A smaller party musters everyone it has.
    EXPECT_EQ(DcOculusDriver::MusterQuorum(3), 3u);
    EXPECT_EQ(DcOculusDriver::MusterQuorum(5), 5u);
    EXPECT_EQ(DcOculusDriver::MusterQuorum(0), 1u);
}

TEST(Oculus, InTheAirTheLegContinuesUntilTheTankLands)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.tankIsland = SITE_NONE;  // between islands
    in.mounted = 4;             // one fell behind; the leg is already flying
    in.anyEngaged = true;       // a picket
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Fly);
    EXPECT_EQ(v.action, DriverAction::Hold);

    in.anyEngaged = false;
    in.tankIsland = SITE_R2C;  // over the destination, not yet landed
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Fly);
}

TEST(Oculus, TankHoldsTheSaddleForStragglersThenEveryoneStepsOff)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.tankIsland = SITE_R2C;
    in.tankLanded = true;
    in.mounted = 5;
    in.landed = 3;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::LandWait);
    EXPECT_EQ(v.phase, Phase::Fly) << "nobody steps off yet";
    EXPECT_EQ(v.landWaitSinceMs, NOW);

    in.landed = 5;
    in.landWaitSinceMs = NOW - 1000;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Land);
    EXPECT_EQ(v.phase, Phase::Land);

    in.landed = 3;
    in.landWaitSinceMs = NOW - LAND_WAIT_MS;
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Land) << "not forever";
}

TEST(Oculus, OnSiteYieldsToTheObjectiveAndLandsTheRest)
{
    DcOculusDriver::Inputs in = OnDrakosRing();
    in.tankIsland = SITE_R2C;
    in.mounted = 2;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::OnSite);
    EXPECT_EQ(v.action, DriverAction::Yield) << "the island clear is the objective's";
    EXPECT_EQ(v.phase, Phase::Land) << "the two still aloft come down";

    in.mounted = 0;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Idle);
    EXPECT_EQ(v.action, DriverAction::Yield);
}

TEST(Oculus, DeadTankLandsTheRidersAlreadyAloft)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.tankAlive = false;
    in.tankMounted = false;
    in.tankIsland = SITE_NONE;
    in.mounted = 3;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Down);
    EXPECT_EQ(v.phase, Phase::Land) << "a follower normally waits for the tank on foot at the site";

    in.mounted = 0;
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Idle) << "nobody starts a leg without the tank";
}

TEST(Oculus, TheStageBecomesTheEregosFight)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.dest = SITE_R4;
    in.destHover = true;
    in.tankIsland = SITE_NONE;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.phase, Phase::Fly);
    EXPECT_TRUE(v.hover);

    in.tankAtStage = true;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Eregos);
    EXPECT_EQ(v.phase, Phase::Eregos);

    // Engaged, the air is the fight whoever is left in the saddle.
    in.tankAtStage = false;
    in.tankAlive = false;
    in.tankMounted = false;
    in.eregosEngaged = true;
    in.mounted = 2;
    EXPECT_EQ(DcOculusDriver::Decide(in).phase, Phase::Eregos);
}

TEST(Oculus, AfterEregosTheRidersLandOnTheRingFourFloor)
{
    DcOculusDriver::Inputs in = MountedOnTheRing();
    in.eregosDone = true;
    in.dest = SITE_NONE;
    in.tankIsland = SITE_NONE;
    in.mounted = 5;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.dest, SITE_R4);
    EXPECT_FALSE(v.hover);
    EXPECT_EQ(v.phase, Phase::Fly);

    in.mounted = 0;
    in.tankMounted = false;
    v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Complete);
    EXPECT_TRUE(v.complete);
}

TEST(Oculus, UnreachableCorpseRequestsRegroup)
{
    DcOculusDriver::Inputs in = OnDrakosRing();
    in.tankIsland = SITE_R3IN;
    in.dest = SITE_R4;
    in.destHover = true;
    in.unreachableCorpse = true;
    DcOculusDriver::Verdict v = DcOculusDriver::Decide(in);
    EXPECT_EQ(v.state, State::Regroup);
    EXPECT_EQ(v.action, DriverAction::Regroup);

    in.lastRegroupMs = NOW - 1000;
    EXPECT_NE(DcOculusDriver::Decide(in).action, DriverAction::Regroup) << "once per cooldown";
    in.lastRegroupMs = NOW - REGROUP_COOLDOWN_MS;
    EXPECT_EQ(DcOculusDriver::Decide(in).action, DriverAction::Regroup);

    in.mounted = 1;
    EXPECT_NE(DcOculusDriver::Decide(in).action, DriverAction::Regroup) << "never pull riders out of the air";
    in.mounted = 0;
    in.anyEngaged = true;
    EXPECT_NE(DcOculusDriver::Decide(in).action, DriverAction::Regroup);

    in.anyEngaged = false;
    in.unreachableCorpse = false;
    in.fallen = true;
    EXPECT_EQ(DcOculusDriver::Decide(in).action, DriverAction::Regroup) << "a fall is the same exit";
}

TEST(Oculus, NeverProposesTwoActions)
{
    for (bool drakos : { false, true })
        for (uint8 dest : std::initializer_list<uint8>{ SITE_R2C, SITE_R4, SITE_NONE })
            for (bool alive : { false, true })
                for (bool tankMounted : { false, true })
                    for (uint8 island : std::initializer_list<uint8>{ SITE_R1_GIVERS, SITE_R2C, SITE_R4, SITE_NONE })
                        for (bool landedTank : { false, true })
                            for (uint32 mounted : { 0u, 3u, 5u })
                                for (bool engaged : { false, true })
                                    for (bool eregos : { false, true })
                                        for (bool corpse : { false, true })
                                        {
                                            DcOculusDriver::Inputs in = OnDrakosRing();
                                            in.drakosDone = drakos;
                                            in.dest = dest;
                                            in.destHover = dest == SITE_R4;
                                            in.tankAlive = alive;
                                            in.tankMounted = alive && tankMounted;
                                            in.tankIsland = island;
                                            in.tankLanded = in.tankMounted && landedTank;
                                            in.mounted = mounted;
                                            in.landed = landedTank ? mounted : 0;
                                            in.anyEngaged = engaged;
                                            in.eregosEngaged = eregos;
                                            in.unreachableCorpse = corpse;
                                            DcOculusDriver::Verdict const v = DcOculusDriver::Decide(in);

                                            EXPECT_EQ(v.action == DriverAction::Regroup, v.state == State::Regroup);
                                            if (v.complete)
                                                EXPECT_EQ(v.state, State::Complete);
                                            bool const flying = v.phase == Phase::Fly || v.phase == Phase::Land ||
                                                                v.phase == Phase::Eregos;
                                            if (flying)
                                                EXPECT_TRUE(mounted > 0 || in.tankMounted)
                                                    << "a flying phase with nobody to fly it";
                                            // THE LIFT-OFF INVARIANT: a mounted tank still over an
                                            // island that is not its destination never gets Fly while
                                            // anyone is engaged.
                                            bool const onStart = in.tankMounted && island != SITE_NONE &&
                                                                 island != (dest == SITE_NONE ? SITE_R4 : dest);
                                            if (onStart && engaged && !eregos)
                                                EXPECT_NE(v.phase, Phase::Fly) << "lift-off in combat";
                                            if (!drakos)
                                                EXPECT_EQ(v.action, DriverAction::Yield);
                                        }
}

// ===========================================================================
//  2. THE RIDER AND THE FLIGHT
// ===========================================================================

TEST(Oculus, EssenceColourFollowsRole)
{
    EXPECT_EQ(ColourForRole(/*tank*/ true, /*healer*/ false), Colour::Ruby);
    EXPECT_EQ(ColourForRole(true, true), Colour::Ruby) << "a tank is the Ruby first";
    EXPECT_EQ(ColourForRole(false, true), Colour::Emerald);
    EXPECT_EQ(ColourForRole(false, false), Colour::Amber);

    // oculus.cpp: the giver, the essence it stores, the item's Call spell, the drake.
    ColourRow const& ruby = RowFor(Colour::Ruby);
    EXPECT_EQ(ruby.giver, 27658u);
    EXPECT_EQ(ruby.essence, 37860u);
    EXPECT_EQ(ruby.callSpell, 49462u);
    EXPECT_EQ(ruby.drake, 27756u);
    EXPECT_EQ(ruby.gossipOption, 0) << "his only item opens submenu 9575; the drill-down takes its first";

    ColourRow const& amber = RowFor(Colour::Amber);
    EXPECT_EQ(amber.giver, 27659u);
    EXPECT_EQ(amber.essence, 37859u);
    EXPECT_EQ(amber.callSpell, 49461u);
    EXPECT_EQ(amber.drake, 27755u);
    EXPECT_EQ(amber.gossipOption, 1) << "item 0 is lore; item 1 is the give/swap option";

    ColourRow const& emerald = RowFor(Colour::Emerald);
    EXPECT_EQ(emerald.giver, 27657u);
    EXPECT_EQ(emerald.essence, 37815u);
    EXPECT_EQ(emerald.callSpell, 49345u);
    EXPECT_EQ(emerald.drake, 27692u);
    EXPECT_EQ(emerald.gossipOption, 1);

    // oculus.h's *POS: where npc_oculus_drakegiverAI walks each giver once Drakos dies.
    EXPECT_FLOAT_EQ(ruby.postX, 941.355f);
    EXPECT_FLOAT_EQ(ruby.postY, 1044.26f);
    EXPECT_FLOAT_EQ(amber.postX, 943.202f);
    EXPECT_FLOAT_EQ(amber.postY, 1059.35f);
    EXPECT_FLOAT_EQ(emerald.postX, 949.056f);
    EXPECT_FLOAT_EQ(emerald.postY, 1032.97f);
    for (ColourRow const& c : COLOURS)
        EXPECT_FLOAT_EQ(c.postZ, 359.967f) << c.name;
}

// Live tp-20260913-003200-1 (tr-…-3): the tank began the muster 63.9yd from
// Belgaristrasz, outside GIVER_SCAN, so it read "no giver" and waited where it
// stood; the on-foot tank's muster has no timeout, and four mounted riders sat on
// Drakos's ring for eleven minutes.
TEST(Oculus, ARiderThatCannotSeeItsGiverWalksToItsPostAndIsNeverHeldForEver)
{
    DcOculusRider::Inputs in = Flying();
    in.phase = Phase::Muster;
    in.isTank = true;
    in.onVehicle = false;
    in.onOcDrake = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = false;
    in.giverPresent = false;
    in.postDist = 63.9f;

    DcOculusRider::Verdict v = DcOculusRider::Decide(in);
    EXPECT_EQ(v.action, RiderAction::WalkToGiver) << "out of scan is not absent: walk to the post";
    EXPECT_EQ(v.essenceWaitSinceMs, NOW) << "the wait clock starts on the first tick without an essence";

    in.postDist = 2.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::WaitForGiver)
        << "at the post with the giver still walking out: wait";

    // The clock round-trips; inside the budget the giver still gets its chance.
    in.essenceWaitSinceMs = NOW - (GIVER_WAIT_MS - 1);
    v = DcOculusRider::Decide(in);
    EXPECT_EQ(v.action, RiderAction::WaitForGiver);
    EXPECT_EQ(v.essenceWaitSinceMs, NOW - (GIVER_WAIT_MS - 1));

    // Past it, whatever the giver is doing, the essence is handed over.
    in.essenceWaitSinceMs = NOW - GIVER_WAIT_MS;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::FabricateEssence);
    in.giverPresent = true;
    in.giverDist = 3.0f;
    in.giverFlagged = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::FabricateEssence)
        << "a flagless giver in reach does not hold the muster either";

    // With the essence in the bags the clock is gone.
    in.hasEssence = true;
    v = DcOculusRider::Decide(in);
    EXPECT_EQ(v.action, RiderAction::Mount);
    EXPECT_EQ(v.essenceWaitSinceMs, 0u);
}

TEST(Oculus, RiderOnFootGetsItsEssenceThenMountsThenHoldsStill)
{
    DcOculusRider::Inputs in = Flying();
    in.phase = Phase::Muster;
    in.onVehicle = false;
    in.onOcDrake = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = false;
    in.giverPresent = true;
    in.giverFlagged = true;
    in.giverDist = 20.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::WalkToGiver);

    in.giverDist = 3.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Gossip);

    in.giverFlagged = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::WaitForGiver)
        << "a flagless giver in reach: wait (bounded by GIVER_WAIT_MS)";

    in.hasEssence = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Mount);

    in.essenceOnCooldown = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);

    in.mountIssuedMs = NOW - 1000;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::SettleHold)
        << "moving inside the settle burns the cooldown with no drake";
    in.engaged = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::SettleHold);
    in.mountIssuedMs = NOW - MOUNT_SETTLE_MS;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None) << "never mount into a fight";

    // Off Drakos's ring without an essence: handed over.
    in = Flying();
    in.onVehicle = false;
    in.island = SITE_R2S;
    in.hasEssence = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::FabricateEssence);

    // Already standing on the destination: nothing to do.
    in.island = SITE_R2C;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
}

TEST(Oculus, LegIssuesOneWaypointAndAdvancesOnArrival)
{
    OcSite const& giv = SITES[SITE_R1_GIVERS];
    OcSite const& r2c = SITES[SITE_R2C];
    Vec const start{ giv.padX, giv.padY, giv.padZ + 1.0f };

    DcOculusFlight::LegPlan const leg = DcOculusFlight::PlanLeg(start, SITE_R1_GIVERS, SITE_R2C, 0, true, kClear);
    ASSERT_EQ(leg.count, 3u) << "rise, cruise, descend";
    EXPECT_FALSE(leg.blocked);
    EXPECT_FALSE(leg.viaColumn);

    // Rise in place to the cruise altitude: max of the two hovers.
    EXPECT_FLOAT_EQ(leg.wp[0].x, start.x);
    EXPECT_FLOAT_EQ(leg.wp[0].y, start.y);
    EXPECT_FLOAT_EQ(leg.wp[0].z, r2c.hoverZ);
    // Across at that altitude to the pad.
    EXPECT_FLOAT_EQ(leg.wp[1].x, r2c.padX);
    EXPECT_FLOAT_EQ(leg.wp[1].y, r2c.padY);
    EXPECT_FLOAT_EQ(leg.wp[1].z, r2c.hoverZ);
    // Down to LAND_HOVER over it.
    EXPECT_FLOAT_EQ(leg.wp[2].z, r2c.padZ + LAND_HOVER);

    DcOculusFlight::Step s = DcOculusFlight::Advance(start, leg, 0);
    EXPECT_FALSE(s.arrived);
    EXPECT_EQ(s.cursor, 0u);

    s = DcOculusFlight::Advance(leg.wp[0], leg, s.cursor);
    EXPECT_EQ(s.cursor, 1u) << "at the top of the climb, cruise";
    EXPECT_FLOAT_EQ(s.target.x, leg.wp[1].x);

    s = DcOculusFlight::Advance(leg.wp[1], leg, s.cursor);
    EXPECT_EQ(s.cursor, 2u);

    s = DcOculusFlight::Advance(leg.wp[2], leg, s.cursor);
    EXPECT_TRUE(s.arrived);

    // A hover-only site ends level at its hover altitude, not on the floor.
    OcSite const& r4 = SITES[SITE_R4];
    DcOculusFlight::LegPlan const up =
        DcOculusFlight::PlanLeg({ 1118.0f, 1080.0f, 510.0f }, SITE_R3IN, SITE_R4, 0, false, kClear);
    EXPECT_FLOAT_EQ(up.wp[up.count - 1].z, r4.hoverZ);
}

TEST(Oculus, ABlockedClimbGoesUpTheColumn)
{
    // Urom's arena has the Ring 4 floor overhead: straight up is blocked.
    Vec const start{ 1118.31f, 1080.38f, 510.0f };
    auto const roofed = [&start](Vec const& a, Vec const& b)
    {
        bool const vertical = std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f;
        bool const overArena = std::hypot(a.x - start.x, a.y - start.y) < 1.0f;
        return !(vertical && overArena && std::max(a.z, b.z) > 600.0f);
    };
    DcOculusFlight::LegPlan const leg = DcOculusFlight::PlanLeg(start, SITE_R3IN, SITE_R4, 0, false, roofed);
    EXPECT_TRUE(leg.viaColumn);
    EXPECT_FALSE(leg.blocked) << "the slide and the shaft are clear";
    ASSERT_GE(leg.count, 4u);
    EXPECT_FLOAT_EQ(leg.wp[0].z, start.z + COLUMN_SLIDE_Z);
    EXPECT_FLOAT_EQ(leg.wp[1].x, SHAFT_X);
    EXPECT_FLOAT_EQ(leg.wp[1].y, SHAFT_Y);
    EXPECT_FLOAT_EQ(leg.wp[2].x, SHAFT_X);
    EXPECT_GE(leg.wp[2].z, SITES[SITE_R4].hoverZ);

    // A chord that nothing clears is reported.
    auto const walls = [](Vec const&, Vec const&) { return false; };
    EXPECT_TRUE(DcOculusFlight::PlanLeg(start, SITE_R3IN, SITE_R4, 0, false, walls).blocked);
}

TEST(Oculus, LaneOffsetsAreDistinctAndInsideTheIsland)
{
    for (uint8 site = 0; site < SITE_COUNT; ++site)
    {
        OcSite const& r = SITES[site];
        std::vector<Vec> pts;
        for (uint32 lane = 0; lane < 5; ++lane)
        {
            Vec const p = DcOculusFlight::PadLanePoint(site, lane);
            EXPECT_LE(std::hypot(p.x - r.padX, p.y - r.padY), r.landRadius + 0.01f) << r.name << " lane " << lane;
            EXPECT_EQ(DcOculusFlight::IslandOf(p.x, p.y, p.z), site) << r.name << " lane " << lane;
            for (Vec const& q : pts)
                EXPECT_GT(std::hypot(p.x - q.x, p.y - q.y), 1.5f) << r.name << " lane " << lane << " stacks";
            pts.push_back(p);
        }
    }

    // The cruise lanes: alternating sides, and never the same altitude.
    Vec const a{ 0, 0, 0 }, b{ 100, 0, 0 };
    Vec const l1 = DcOculusFlight::LaneOffset(a, b, 1), l2 = DcOculusFlight::LaneOffset(a, b, 2);
    EXPECT_GT(l1.y * l2.y, -1.0f * LANE_SPACING * LANE_SPACING - 1.0f);
    EXPECT_LT(l1.y * l2.y, 0.0f) << "lanes 1 and 2 fly either side of the chord";
    EXPECT_NE(l1.z, l2.z);
    Vec const l0 = DcOculusFlight::LaneOffset(a, b, 0);
    EXPECT_FLOAT_EQ(l0.x, 0.0f);
    EXPECT_FLOAT_EQ(l0.y, 0.0f);
}

TEST(Oculus, TheLandingGateIsTheOnlyDismountGate)
{
    OcSite const& r2s = SITES[SITE_R2S];
    Vec const snap{ r2s.padX, r2s.padY, r2s.padZ };
    Vec base{ r2s.padX, r2s.padY, r2s.padZ + LAND_HOVER };
    EXPECT_TRUE(DcOculusFlight::Landed(base, true, snap, SITE_R2S));
    EXPECT_FALSE(DcOculusFlight::Landed(base, false, snap, SITE_R2S)) << "no poly below";
    EXPECT_FALSE(DcOculusFlight::Landed(base, true, snap, SITE_R2N)) << "the wrong island";

    base.z = r2s.padZ + LAND_TOLERANCE + 0.5f;
    EXPECT_FALSE(DcOculusFlight::Landed(base, true, snap, SITE_R2S)) << "still in the air";

    base = { r2s.padX + LAND_SNAP_2D + 1.0f, r2s.padY, r2s.padZ + 1.0f };
    EXPECT_FALSE(DcOculusFlight::Landed(base, true, snap, SITE_R2S)) << "beside the snap, over the rim";
}

TEST(Oculus, DismountRequiresLandedCheck)
{
    DcOculusRider::Inputs in = Flying();
    in.legArrived = true;
    in.phase = Phase::Land;
    in.landed = false;
    EXPECT_NE(DcOculusRider::Decide(in).action, RiderAction::Dismount);

    in.landed = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount);

    // In Fly a follower steps off only once the tank is on foot at the site...
    in.phase = Phase::Fly;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
    in.tankOnFootOnDest = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount);
    // ...and the tank itself only in Land.
    in.isTank = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);

    // THE INVARIANT, swept.
    for (Phase phase : { Phase::Idle, Phase::Muster, Phase::Fly, Phase::Land, Phase::Eregos })
        for (bool arrived : { false, true })
            for (bool landed : { false, true })
                for (bool stalled : { false, true })
                    for (bool reissued : { false, true })
                        for (bool tank : { false, true })
                            for (bool tankDown : { false, true })
                                for (bool fresh : { false, true })
                                {
                                    // 0 nobody on the drake, 1 a picket, 2 a ground mob
                                    for (int fire = 0; fire < 3; ++fire)
                                    {
                                        DcOculusRider::Inputs s = Flying();
                                        s.phase = phase;
                                        s.planFresh = fresh;
                                        s.legArrived = arrived;
                                        s.landed = landed;
                                        s.legStalled = stalled;
                                        s.stallReissued = reissued;
                                        s.isTank = tank;
                                        s.tankOnFootOnDest = tankDown;
                                        s.baseInCombat = fire > 0;
                                        s.groundAttacker = fire > 1;
                                        RiderAction const a = DcOculusRider::Decide(s).action;
                                        if (a == RiderAction::Dismount)
                                            EXPECT_TRUE(landed) << "dismounted in the air";
                                        EXPECT_NE(a, RiderAction::Mount) << "mounting from the saddle";
                                    }
                                }
}

TEST(Oculus, StalledLegReissuesThenNeverDismountsInAir)
{
    DcOculusRider::Inputs in = Flying();
    in.legStalled = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Reissue);

    in.stallReissued = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Nudge) << "climb and re-plan, never step off";

    in.landed = true;
    in.phase = Phase::Land;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount) << "wedged on the pad: step off";

    // A picket on the drake is a hover, not a stall.
    in = Flying();
    in.baseInCombat = true;
    in.legStalled = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
}

TEST(Oculus, ALaneThatWillNotLandFallsBackToThePadCentre)
{
    DcOculusRider::Inputs in = Flying();
    in.legArrived = true;
    in.landed = false;
    DcOculusRider::Verdict v = DcOculusRider::Decide(in);
    EXPECT_EQ(v.action, RiderAction::None);
    EXPECT_EQ(v.arrivedSinceMs, NOW);

    in.arrivedSinceMs = NOW - PAD_FALLBACK_MS;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::FlyPadCentre);
    in.atPadCentre = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
}

TEST(Oculus, EregosStationIsFiftyYardsOnOwnBearing)
{
    Vec const eregos{ 1077.04f, 1086.21f, 655.5f };
    std::set<int> bearings;
    for (uint32 lane = 0; lane < 5; ++lane)
    {
        Vec const s = DcOculusFlight::EregosStation(eregos, lane, false, EREGOS_STATION_RANGE);
        EXPECT_NEAR(std::hypot(s.x - eregos.x, s.y - eregos.y), EREGOS_STATION_RANGE, 0.05f) << "lane " << lane;
        EXPECT_LE(std::fabs(s.z - eregos.z), LANE_Z * 3.0f) << "lane " << lane;
        float const deg = std::atan2(s.y - eregos.y, s.x - eregos.x) * 180.0f / DcOculusFlight::kPi;
        bearings.insert(static_cast<int>(std::lround(deg)));
    }
    EXPECT_EQ(bearings.size(), 5u) << "every rider on its own bearing";
    // Adjacent lanes 36 degrees apart: lane 1 and lane 0.
    Vec const s0 = DcOculusFlight::EregosStation(eregos, 0, false, EREGOS_STATION_RANGE);
    Vec const s1 = DcOculusFlight::EregosStation(eregos, 1, false, EREGOS_STATION_RANGE);
    float const a0 = std::atan2(s0.y - eregos.y, s0.x - eregos.x);
    float const a1 = std::atan2(s1.y - eregos.y, s1.x - eregos.x);
    float d = std::fabs(a1 - a0) * 180.0f / DcOculusFlight::kPi;
    if (d > 180.0f)
        d = 360.0f - d;
    EXPECT_NEAR(d, EREGOS_LANE_DEG, 0.1f);
}

TEST(Oculus, PlanarShiftScattersOutward)
{
    Vec const eregos{ 1077.04f, 1086.21f, 655.5f };
    for (uint32 lane = 0; lane < 5; ++lane)
    {
        Vec const calm = DcOculusFlight::EregosStation(eregos, lane, false, EREGOS_STATION_RANGE);
        Vec const shift = DcOculusFlight::EregosStation(eregos, lane, true, EREGOS_STATION_RANGE);
        EXPECT_NEAR(std::hypot(shift.x - eregos.x, shift.y - eregos.y),
                    EREGOS_STATION_RANGE + PLANAR_SCATTER_OUT, 0.05f);
        EXPECT_NEAR(shift.z - calm.z, PLANAR_SCATTER_UP, 0.01f);
        // Same bearing: outward, not sideways.
        EXPECT_NEAR(std::atan2(shift.y - eregos.y, shift.x - eregos.x),
                    std::atan2(calm.y - eregos.y, calm.x - eregos.x), 0.001f);
    }
}

TEST(Oculus, RiderStationsOnEregosAndTheTankPullsHim)
{
    DcOculusRider::Inputs in = Flying();
    in.phase = Phase::Eregos;
    in.dest = SITE_R4;
    in.eregosPresent = true;
    in.eregosAttackable = true;
    in.nearEregos = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Fly) << "up to the stage before any station";

    in.nearEregos = true;
    in.stationError = 20.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Station);
    in.stationError = 3.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None) << "on station: the rotation's tick";

    in.isTank = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::PullEregos);
    in.eregosEngaged = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);

    in.stationError = 20.0f;
    in.channeling = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None) << "a move cancels Temporal Rift / Dream Funnel";

    // Engaged Eregos is answered even on a stale plan phase.
    in = Flying();
    in.phase = Phase::Fly;
    in.eregosPresent = true;
    in.eregosEngaged = true;
    in.nearEregos = true;
    in.stationError = 20.0f;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Station);
}

// ===========================================================================
//  3. THE AUTHORED DATA
// ===========================================================================

TEST(Oculus, RosterIsThreeBossRowsAndEightObjectivesInOrder)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->gate, DcDifficultyGate::Any) << "both difficulties credit the normal entries";

    std::vector<uint32> const removed(p->remove.begin(), p->remove.end());
    EXPECT_EQ(removed, (std::vector<uint32>{ NPC_VAROS, NPC_UROM, NPC_EREGOS }));
    ASSERT_EQ(p->reorder.size(), 1u);
    EXPECT_EQ(p->reorder[0].first, NPC_DRAKOS);
    EXPECT_EQ(p->reorder[0].second, ORDER_DRAKOS);

    uint32 bosses = 0, objectives = 0;
    std::set<int32> orders = { ORDER_DRAKOS };
    for (DungeonBossInfo const& b : p->add)
    {
        if (b.kind == DungeonAnchorKind::Boss)
            ++bosses;
        else
        {
            ++objectives;
            EXPECT_EQ(b.encounterIndex, 0u) << b.name << ": a real bit would self-complete on the credit";
            EXPECT_EQ(b.onArriveHook, 0u) << b.name;
        }
        orders.insert(b.orderOverride);
    }
    EXPECT_EQ(bosses, 2u) << "Varos and Urom, re-anchored (Drakos stays derived)";
    EXPECT_EQ(objectives, 8u);
    EXPECT_EQ(orders, (std::set<int32>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }));

    struct Row { uint32 entry; int32 order; uint32 eventId; };
    constexpr Row kRows[] = {
        { DcRoster::OBJ(EVENT_PORTAL), ORDER_PORTAL, EVENT_PORTAL },
        { DcRoster::OBJ(EVENT_R2C), ORDER_R2C, EVENT_R2C },
        { DcRoster::OBJ(EVENT_R2S), ORDER_R2S, EVENT_R2S },
        { DcRoster::OBJ(EVENT_R2N), ORDER_R2N, EVENT_R2N },
        { NPC_VAROS, ORDER_VAROS, 0 },
        { DcRoster::OBJ(EVENT_UROM_P0), ORDER_UROM_P0, EVENT_UROM_P0 },
        { DcRoster::OBJ(EVENT_UROM_P1), ORDER_UROM_P1, EVENT_UROM_P1 },
        { DcRoster::OBJ(EVENT_UROM_P2), ORDER_UROM_P2, EVENT_UROM_P2 },
        { NPC_UROM, ORDER_UROM, 0 },
        { DcRoster::OBJ(EVENT_EREGOS), ORDER_EREGOS, EVENT_EREGOS },
    };
    for (Row const& r : kRows)
    {
        DungeonBossInfo const* b = Added(*p, r.entry);
        ASSERT_NE(b, nullptr) << "entry " << r.entry;
        EXPECT_EQ(b->orderOverride, r.order) << b->name;
        EXPECT_EQ(b->eventId, r.eventId) << b->name;
        EXPECT_EQ(b->mapId, MAP_ID) << b->name;
    }

    uint32 patches = 0;
    for (BossRosterPatch const& q : BossRosterRegistry::AllPatches())
        if (q.mapId == MAP_ID)
            ++patches;
    EXPECT_EQ(patches, 1u);
}

TEST(Oculus, PortalObjectiveIsFirstAndLandsAtTheSpellDestination)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr);
    DungeonBossInfo const* portal = Added(*p, DcRoster::OBJ(EVENT_PORTAL));
    ASSERT_NE(portal, nullptr);
    EXPECT_EQ(portal->orderOverride, 1);
    EXPECT_FLOAT_EQ(portal->x, 1045.57f);
    EXPECT_FLOAT_EQ(portal->y, 1104.24f);
    EXPECT_GT(std::hypot(portal->x - ORB_X, portal->y - ORB_Y), 100.0f)
        << "anchored on the Orb of the Nexus, the EXIT to Coldarra";

    DungeonEvent const* ev = Ev(EVENT_PORTAL);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& s = ev->steps[0];
    EXPECT_EQ(s.kind, EventStepKind::TeleportParty);
    EXPECT_FLOAT_EQ(s.x, PORTAL_X);
    EXPECT_FLOAT_EQ(s.y, PORTAL_Y);
    // spell_target_position 49305: (983.108, 1054.51, 359.967).
    EXPECT_FLOAT_EQ(s.landX, 983.108f);
    EXPECT_FLOAT_EQ(s.landY, 1054.51f);
    EXPECT_FLOAT_EQ(s.landZ, 359.967f);
    EXPECT_LE(s.radius, portal->arriveRadius) << "a checkpoint radius under the arrival is the leading-step deadlock";
    EXPECT_EQ(DcOculusFlight::IslandOf(s.landX, s.landY, s.landZ), SITE_R1_GIVERS);
    EXPECT_EQ(DcOculusFlight::IslandOf(s.x, s.y, s.z), SITE_R1_ENTRY);
}

TEST(Oculus, EregosDerivedRowIsRemovedAndReplacedOnTheFloor)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr);
    EXPECT_NE(std::find(p->remove.begin(), p->remove.end(), NPC_EREGOS), p->remove.end());
    EXPECT_EQ(Added(*p, NPC_EREGOS), nullptr) << "a row 54yd above the mesh never snaps";

    DungeonBossInfo const* obj = Added(*p, DcRoster::OBJ(EVENT_EREGOS));
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(DcOculusFlight::IslandOf(obj->x, obj->y, obj->z), SITE_R4);
    EXPECT_LT(655.5f - obj->z, 60.0f);
}

TEST(Oculus, VarosAndUromAreAnchoredWhereTheyAreFought)
{
    BossRosterPatch const* p = Patch();
    ASSERT_NE(p, nullptr);

    DungeonBossInfo const* varos = Added(*p, NPC_VAROS);
    ASSERT_NE(varos, nullptr);
    EXPECT_EQ(varos->kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(varos->inheritCompletionFrom, NPC_VAROS) << "his own kill bit";
    EXPECT_EQ(DcOculusFlight::IslandOf(varos->x, varos->y, varos->z), SITE_R2V);
    EXPECT_LT(std::hypot(varos->x - 1285.60f, varos->y - 1070.36f), 40.0f);

    DungeonBossInfo const* urom = Added(*p, NPC_UROM);
    ASSERT_NE(urom, nullptr);
    EXPECT_EQ(urom->inheritCompletionFrom, NPC_UROM);
    EXPECT_FLOAT_EQ(urom->x, UROM_CORDS[3].x);
    EXPECT_FLOAT_EQ(urom->y, UROM_CORDS[3].y);
    EXPECT_EQ(DcOculusFlight::IslandOf(urom->x, urom->y, urom->z), SITE_R3IN);
    // boss_urom: AttackStart only within 55yd of (1103, 1049, 510).
    EXPECT_LT(std::hypot(urom->x - 1103.0f, urom->y - 1049.0f), 55.0f);
}

TEST(Oculus, EveryPadIsOnItsIslandSide)
{
    for (uint8 site = 0; site < SITE_COUNT; ++site)
    {
        OcSite const& r = SITES[site];
        EXPECT_EQ(DcOculusFlight::IslandOf(r.padX, r.padY, r.padZ), site) << r.name;
        EXPECT_EQ(DcOculusFlight::IslandOf(r.padX, r.padY, r.hoverZ), SITE_NONE)
            << r.name << ": a drake at its hover altitude must read as in the air";
        if (r.clearRadius > 0.0f)
            EXPECT_EQ(DcOculusFlight::IslandOf(r.clearX, r.clearY, r.clearZ), site) << r.name;
    }
    // (The island circles are coarse — a ring's hollow centre reads as that ring —
    // so whether the central shaft is really open is a navmesh question, asked by
    // OculusRouteProbe.TheCentralShaftIsOpenAtEveryRing.)
}

TEST(Oculus, HoverAltitudesSitBetweenRings)
{
    for (uint8 ring = 1; ring <= 4; ++ring)
    {
        float topOfRing = -1.0f, bottomOfNext = 100000.0f, hover = 0.0f;
        for (OcSite const& r : SITES)
        {
            if (r.ring == ring)
            {
                topOfRing = std::max(topOfRing, r.padZ);
                hover = r.hoverZ;
            }
            if (r.ring == ring + 1)
                bottomOfNext = std::min(bottomOfNext, r.padZ);
        }
        for (OcSite const& r : SITES)
            if (r.ring == ring)
                EXPECT_FLOAT_EQ(r.hoverZ, hover) << r.name << ": one hover altitude per ring";
        EXPECT_GT(hover, topOfRing + 10.0f) << "ring " << int(ring);
        if (ring < 4)
            EXPECT_LT(hover, bottomOfNext - 10.0f) << "ring " << int(ring);
    }
}

TEST(Oculus, SiteForRowCoversEveryFlightRow)
{
    using DcOculusDriver::SiteForRow;
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_PORTAL)), SITE_NONE) << "on foot";
    EXPECT_EQ(SiteForRow(NPC_DRAKOS), SITE_NONE) << "on foot";
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_R2C)), SITE_R2C);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_R2S)), SITE_R2S);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_R2N)), SITE_R2N);
    EXPECT_EQ(SiteForRow(NPC_VAROS), SITE_R2V);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_UROM_P0)), SITE_R3P0);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_UROM_P1)), SITE_R3P1);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_UROM_P2)), SITE_R3P2);
    EXPECT_EQ(SiteForRow(NPC_UROM), SITE_R3IN);
    EXPECT_EQ(SiteForRow(DcRoster::OBJ(EVENT_EREGOS)), SITE_R4);
}

TEST(Oculus, FlyingSafeSetIsExactlyThree)
{
    std::set<uint32> const safe(std::begin(FLYING_SAFE_ENTRIES), std::end(FLYING_SAFE_ENTRIES));
    EXPECT_EQ(safe, (std::set<uint32>{ 27638, 27656, 28276 }));
}

TEST(Oculus, StandDownTableHasEregosAndExemptsTheRider)
{
    DcBossStandDown::TableRow const* row = DcBossStandDown::FindTableRow(MAP_ID);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->bossEntry, NPC_EREGOS);
    EXPECT_EQ(row->guidSlot, DATA_EREGOS);
    EXPECT_EQ(DcBossStandDown::FindTableRow(650), nullptr);

    EXPECT_EQ(DcBossStandDown::ClassifyAction("dungeon clear oc rider", true),
              DcBossStandDown::ActionVerdict::Stock)
        << "the Eregos fight is fought entirely from the rider rung";
    EXPECT_EQ(DcBossStandDown::ClassifyAction("dungeon clear pull maneuver", true),
              DcBossStandDown::ActionVerdict::Inert);
}

TEST(Oculus, PurgeRowsCoverGuardianAndWhelp)
{
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_ID, NPC_AZURE_RING_GUARDIAN, false));
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_ID, NPC_GREATER_LEY_WHELP, false))
        << "never out of a live Eregos fight";
    EXPECT_TRUE(DcCombatPurgeRegistry::IsPurgeable(MAP_ID, NPC_GREATER_LEY_WHELP, true));
    EXPECT_FALSE(DcCombatPurgeRegistry::IsPurgeable(MAP_ID, NPC_CENTRIFUGE_CONSTRUCT, false));
}

TEST(Oculus, UnstableSphereIsALeaveOnceHazard)
{
    DcHazardEmitter const* e = DcHazardRegistry::Find(MAP_ID, NPC_UNSTABLE_SPHERE);
    ASSERT_NE(e, nullptr);
    EXPECT_GT(e->vacateRadius, 0.0f);
    EXPECT_GT(e->retreatSlack, e->holdBand);
    EXPECT_GT(e->vacateRadius + e->retreatSlack, e->radius) << "the retreat must aim outside its own keep-out";
}

TEST(Oculus, EventFlagsAreAsAuthored)
{
    for (uint32 id = EVENT_PORTAL; id <= EVENT_DRIVER; ++id)
        ASSERT_NE(Ev(id), nullptr) << "event " << id << " is missing";

    for (uint32 id = EVENT_PORTAL; id <= EVENT_EREGOS; ++id)
    {
        DungeonEvent const* ev = Ev(id);
        EXPECT_EQ(ev->activation, EventActivation::Anchored) << "event " << id;
        EXPECT_EQ(ev->gate, DcDifficultyGate::Any) << "event " << id;
        EXPECT_TRUE(ev->persistent) << "event " << id;
        EXPECT_TRUE(ev->required) << "event " << id;
        EXPECT_FALSE(ev->repeatable) << "event " << id;
        EXPECT_FALSE(ev->drivesInCombat) << "event " << id;
        EXPECT_FALSE(ev->stepsOwnMovement) << "event " << id;
    }

    DungeonEvent const* d = Ev(EVENT_DRIVER);
    EXPECT_EQ(d->activation, EventActivation::Conditional);
    EXPECT_TRUE(d->condition);
    EXPECT_TRUE(d->repeatable);
    EXPECT_TRUE(d->persistent);
    EXPECT_FALSE(d->required);
    EXPECT_TRUE(d->ownsThePull);
    EXPECT_TRUE(d->yieldsTheApproach)
        << "tr-20260913-003200-10: the 'on site' yield and the off-line rejoin's stand-down "
           "waited on each other 3yd outside the south pad's arrival";
    EXPECT_TRUE(d->drivesInCombat);
    EXPECT_TRUE(d->stepsOwnMovement);
    ASSERT_EQ(d->steps.size(), 1u);
    EXPECT_EQ(d->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(d->steps[0].hookId, HOOK_OC_DRIVER);
    EXPECT_EQ(d->steps[0].timeoutMs, DRIVER_TIMEOUT_MS);

    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_OC_DRIVER));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_OC_EREGOS));
    EXPECT_NE(HOOK_OC_DRIVER, HOOK_OC_EREGOS) << "a Custom step's hook gets a dummy BossInfo";
}

TEST(Oculus, SiteClearsArriveOnThePadThenClearTheIsland)
{
    struct Row { uint32 id; uint8 site; bool filtered; bool guard; bool poke; };
    // (The central ring has no whole-island clear: TheCentralRingIsSweptRoundTheHorseshoe.)
    constexpr Row kRows[] = {
        { EVENT_R2S, SITE_R2S, true, false, false },
        { EVENT_R2N, SITE_R2N, true, true, false },
        { EVENT_UROM_P0, SITE_R3P0, false, false, true },
        { EVENT_UROM_P1, SITE_R3P1, false, false, true },
        { EVENT_UROM_P2, SITE_R3P2, false, false, true },
    };
    for (Row const& r : kRows)
    {
        DungeonEvent const* ev = Ev(r.id);
        ASSERT_NE(ev, nullptr);
        OcSite const& s = SITES[r.site];
        ASSERT_EQ(ev->steps.size(), 2u + (r.guard ? 1u : 0u) + (r.poke ? 1u : 0u)) << s.name;

        EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo) << s.name;
        EXPECT_FLOAT_EQ(ev->steps[0].x, s.padX) << s.name;
        EXPECT_FLOAT_EQ(ev->steps[0].radius, OBJ_ARRIVE) << s.name << ": the leading MoveTo is the arrival";

        EventStep const& clear = ev->steps[r.poke ? 2 : 1];
        EXPECT_EQ(clear.kind, EventStepKind::ClearRadius) << s.name;
        EXPECT_FLOAT_EQ(clear.radius, s.clearRadius) << s.name;
        EXPECT_GT(clear.radius, 0.0f) << s.name;
        EXPECT_EQ(!clear.entryFilter.empty(), r.filtered) << s.name;
        if (r.filtered)
            EXPECT_EQ(std::find(clear.entryFilter.begin(), clear.entryFilter.end(),
                                NPC_AZURE_RING_GUARDIAN),
                      clear.entryFilter.end())
                << s.name << ": never clear a flying picket from the ground";

        if (r.guard)
        {
            EXPECT_EQ(ev->steps[2].kind, EventStepKind::MoveTo);
            EXPECT_EQ(ev->steps[2].instanceDataId, static_cast<int32>(DATA_CC_COUNT));
            EXPECT_EQ(ev->steps[2].instanceDataMin, CC_TOTAL);
        }

        // Urom's clears are centred on the cords he summons at.
        if (r.site >= SITE_R3P0 && r.site <= SITE_R3P2)
        {
            Pt3 const& c = UROM_CORDS[r.site - SITE_R3P0];
            EXPECT_LT(std::hypot(clear.x - c.x, clear.y - c.y), 1.0f) << s.name;
            EXPECT_LT(std::hypot(s.padX - c.x, s.padY - c.y), clear.radius) << s.name;
        }
    }

    DungeonEvent const* er = Ev(EVENT_EREGOS);
    ASSERT_NE(er, nullptr);
    ASSERT_EQ(er->steps.size(), 2u);
    EXPECT_EQ(er->steps[1].instanceDataId, static_cast<int32>(DATA_EREGOS));
    EXPECT_EQ(er->steps[1].instanceDataMin, STATE_DONE);
    EXPECT_EQ(er->steps[1].hookId, HOOK_OC_EREGOS);
}

// The construct pads, from the user's live run: the drakes set down in the middle
// of the pack. Each pad is on its platform's rim, every landing lane is out of the
// pack's aggro, the pack is inside the clear, and no hovering picket is over the
// descent (OculusRouteProbe.EachPadRoutesToItsIslandsTargets walks it on foot).
TEST(Oculus, TheConstructPadsLandOnTheRimOutOfAggro)
{
    // detection_range 20, level 79 against level 80: 21yd.
    constexpr float kAggro = 21.0f;
    constexpr float kMargin = 5.0f;
    constexpr float kPicketClear = 30.0f;

    struct Row { uint8 site; std::vector<Pt3> spawns; };
    // `creature`, map 578: three Centrifuge Constructs and a Ring-Lord each.
    std::vector<Row> const rows = {
        { SITE_R2S, { { 1041.2f, 877.6f, 439.5f }, { 1036.5f, 890.5f, 439.5f },
                      { 1023.2f, 893.3f, 439.5f }, { 1045.2f, 899.4f, 439.5f } } },
        { SITE_R2N, { { 1023.7f, 1190.9f, 439.5f }, { 1017.1f, 1202.0f, 439.5f },
                      { 1004.9f, 1203.0f, 439.5f }, { 1025.5f, 1210.9f, 439.5f } } },
    };
    // The Azure Ring Guardians hovering round both platforms.
    constexpr Pt3 kPickets[] = {
        { 1033.3f,  829.7f, 457.0f }, { 1064.1f,  874.5f, 484.5f }, {  992.6f,  884.6f, 464.4f },
        { 1032.8f,  899.4f, 474.2f }, { 1085.1f,  938.9f, 448.5f }, {  984.1f,  939.2f, 442.1f },
        {  979.3f, 1159.7f, 464.1f }, { 1043.4f, 1180.1f, 478.4f }, { 1021.0f, 1200.1f, 504.6f },
        {  999.5f, 1201.6f, 479.3f }, { 1023.3f, 1251.7f, 461.9f },
    };

    for (Row const& r : rows)
    {
        OcSite const& s = SITES[r.site];
        EXPECT_EQ(DcOculusFlight::IslandOf(s.padX, s.padY, s.padZ), r.site) << s.name;
        for (uint32 lane = 0; lane < 5; ++lane)
        {
            Vec const p = DcOculusFlight::PadLanePoint(r.site, lane);
            for (Pt3 const& m : r.spawns)
                EXPECT_GT(std::hypot(p.x - m.x, p.y - m.y), kAggro + kMargin)
                    << s.name << " lane " << lane << " lands in the pack's aggro";
        }
        for (Pt3 const& m : r.spawns)
        {
            EXPECT_LE(std::hypot(m.x - s.clearX, m.y - s.clearY), s.clearRadius) << s.name << ": a mob outside the clear";
            EXPECT_EQ(DcOculusFlight::IslandOf(m.x, m.y, m.z), r.site) << s.name << ": the pack is off the island";
        }
        for (Pt3 const& g : kPickets)
            if (std::fabs(g.z - s.hoverZ) <= 20.0f)
                EXPECT_GE(std::hypot(g.x - s.padX, g.y - s.padY), kPicketClear)
                    << s.name << ": a picket at (" << g.x << ", " << g.y << ") hovers over the descent";
    }
}

// Urom's arena, the same rule: after his third platform he waits at UROM_CORDS[3]
// channelling Evocation, and the drakes used to set down on top of him. Every
// landing lane is out of his reach, on his island, and clear of the pickets.
TEST(Oculus, UromsArenaLandsOutOfHisReach)
{
    // detection_range 20, level 81 against level 80: 21yd.
    constexpr float kAggro = 21.0f;
    constexpr float kMargin = 5.0f;
    constexpr Pt3 kPickets[] = { { 1186.0f, 1058.6f, 556.7f }, { 1041.5f, 1143.4f, 543.8f } };

    OcSite const& s = SITES[SITE_R3IN];
    Pt3 const& urom = UROM_CORDS[3];
    EXPECT_EQ(DcOculusFlight::IslandOf(urom.x, urom.y, urom.z), SITE_R3IN);
    for (uint32 lane = 0; lane < 5; ++lane)
    {
        Vec const p = DcOculusFlight::PadLanePoint(SITE_R3IN, lane);
        EXPECT_EQ(DcOculusFlight::IslandOf(p.x, p.y, p.z), SITE_R3IN) << "lane " << lane;
        EXPECT_GT(std::hypot(p.x - urom.x, p.y - urom.y), kAggro + kMargin) << "lane " << lane << " lands on Urom";
    }
    for (Pt3 const& g : kPickets)
        EXPECT_GE(std::hypot(g.x - s.padX, g.y - s.padY), 30.0f) << "a picket hovers over the descent";
}

// Urom never aggroes (empty MoveInLineOfSight), his pack only spawns once he is
// struck, and a ClearRadius skips roster bosses — so the first live run certified
// every platform empty and flew on without him. Each platform hits him first.
TEST(Oculus, EachUromPlatformHitsHimBeforeItClears)
{
    for (uint8 i = 0; i < 3; ++i)
    {
        DungeonEvent const* ev = Ev(EVENT_UROM_P0 + i);
        ASSERT_NE(ev, nullptr);
        OcSite const& s = SITES[SITE_R3P0 + i];
        ASSERT_EQ(ev->steps.size(), 3u) << s.name;

        EventStep const& poke = ev->steps[1];
        EXPECT_EQ(poke.kind, EventStepKind::KillCreature) << s.name;
        EXPECT_TRUE(poke.engage) << s.name << ": the tank must walk in and hit him";
        EXPECT_EQ(poke.creatureEntry, NPC_UROM) << s.name;
        EXPECT_EQ(ev->steps[2].kind, EventStepKind::ClearRadius) << s.name << ": the pack is cleared after the hit";

        // From anywhere inside the pad's arrival he is inside the search...
        Pt3 const& here = UROM_CORDS[i];
        EXPECT_LT(std::hypot(s.padX - here.x, s.padY - here.y) + OBJ_ARRIVE, poke.radius) << s.name;
        // ...and once teleported on (the next platform, then the arena) he is out
        // of it from anywhere on this island, so the step completes.
        Pt3 const& next = UROM_CORDS[i + 1];
        EXPECT_GT(std::hypot(s.islandX - next.x, s.islandY - next.y) - s.islandRadius, poke.radius) << s.name;
    }
}

// The central ring, from the user's manual run: land at (1045.78, 1067.77, 432.51)
// on one end of the horseshoe's break, then walk round to the other end clearing it.
TEST(Oculus, TheCentralRingIsSweptRoundTheHorseshoe)
{
    OcSite const& r2c = SITES[SITE_R2C];
    EXPECT_NEAR(r2c.padX, 1045.78f, 0.01f);
    EXPECT_NEAR(r2c.padY, 1067.77f, 0.01f);
    EXPECT_NEAR(r2c.padZ, 432.51f, 0.01f);
    EXPECT_FLOAT_EQ(r2c.clearRadius, 0.0f) << "a whole-ring clear judges from the shaft and walks off the rim";

    auto const bearing = [](float x, float y)
    {
        float const b = std::atan2(y - SHAFT_Y, x - SHAFT_X) * 180.0f / DcOculusFlight::kPi;
        return b < 0.0f ? b + 360.0f : b;
    };
    // The break in the floor (OculusRouteProbe.TheCentralRingSweepWalksRoundTheHorseshoe).
    constexpr float kBreakLo = 170.0f, kBreakHi = 190.0f;

    // The landing is on the break's west end. Every stop is on the ring, out of the
    // break, and less than half a turn further round than the one before, and the
    // sweep never walks into the break from the far side.
    float const landing = bearing(r2c.padX, r2c.padY);
    EXPECT_LT(landing, kBreakLo);
    float prev = landing;
    float turned = 0.0f;
    for (OcRingStop const& s : R2C_SWEEP)
    {
        float const b = bearing(s.x, s.y);
        EXPECT_EQ(DcOculusFlight::IslandOf(s.x, s.y, s.z), SITE_R2C) << s.name;
        EXPECT_FALSE(b >= kBreakLo && b <= kBreakHi) << s.name << ": in the break";
        float step = prev - b;
        if (step < 0.0f)
            step += 360.0f;
        EXPECT_GT(step, 1.0f) << s.name;
        EXPECT_LT(step, 180.0f) << s.name;
        turned += step;
        prev = b;
    }
    EXPECT_LT(turned, landing + 360.0f - kBreakHi) << "the sweep walks into the break";

    // The ring's ground spawns (`creature`, map 578): each inside exactly one arc,
    // and the landing well outside every arc.
    constexpr Pt3 kSpawns[] = {
        { 1130.9f, 1097.9f, 433.2f }, { 1137.7f, 1112.0f, 433.2f }, { 1124.4f, 1109.8f, 433.0f },
        { 1112.3f, 1103.7f, 433.2f }, { 1115.4f, 1120.3f, 433.2f }, { 1081.4f, 1111.2f, 433.1f },
        { 1083.8f, 1099.7f, 433.2f }, { 1071.6f, 1103.8f, 433.0f }, { 1061.4f, 1096.0f, 432.5f },
        { 1062.1f,  993.7f, 433.2f }, { 1072.3f, 1007.2f, 433.2f }, { 1076.9f,  994.3f, 433.0f },
        { 1088.2f,  989.9f, 433.2f }, { 1122.4f,  981.5f, 433.2f }, { 1117.6f,  998.6f, 433.2f },
        { 1129.7f,  993.2f, 433.0f }, { 1142.4f,  992.2f, 433.2f }, { 1135.2f, 1005.9f, 433.2f },
    };
    for (Pt3 const& m : kSpawns)
    {
        int arcs = 0;
        for (OcRingStop const& s : R2C_SWEEP)
            if (s.radius > 0.0f && std::hypot(m.x - s.x, m.y - s.y) <= s.radius)
                ++arcs;
        EXPECT_EQ(arcs, 1) << "spawn at (" << m.x << ", " << m.y << ")";
    }
    for (OcRingStop const& s : R2C_SWEEP)
        if (s.radius > 0.0f)
            EXPECT_GT(std::hypot(r2c.padX - s.x, r2c.padY - s.y), s.radius + 15.0f) << s.name;

    // The event: the arrival, then the stops in order, and nothing after them.
    DungeonEvent const* ev = Ev(EVENT_R2C);
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->steps.size(), std::size(R2C_SWEEP) + 1);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(ev->steps[0].x, r2c.padX);
    EXPECT_FLOAT_EQ(ev->steps[0].radius, OBJ_ARRIVE);
    for (std::size_t i = 0; i < std::size(R2C_SWEEP); ++i)
    {
        OcRingStop const& s = R2C_SWEEP[i];
        auto const& step = ev->steps[i + 1];
        bool const arc = s.radius > 0.0f;
        EXPECT_EQ(step.kind, arc ? EventStepKind::ClearRadius : EventStepKind::MoveTo) << s.name;
        EXPECT_FLOAT_EQ(step.x, s.x) << s.name;
        EXPECT_FLOAT_EQ(step.y, s.y) << s.name;
        EXPECT_FLOAT_EQ(step.radius, arc ? s.radius : RING_HOP_ARRIVE) << s.name;
        EXPECT_EQ(step.entryFilter.empty(), !arc) << s.name << ": arcs clear only the ring's ground entries";
        EXPECT_GT(step.timeoutMs, 0u) << s.name;
    }
}

// Live (the central ring, 2026-09-12 22:16): Ring-Lords found the drakes over the
// old pad. Every attacked drake hovered where it was, the landed count stuck at
// three of four for a minute, and nobody stepped off.
TEST(Oculus, UnderGroundFireTheDrakesLandAndStepOff)
{
    // Only a picket holds a drake in the air.
    DcOculusRider::Inputs in = Flying();
    in.baseInCombat = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None) << "a picket: hover and shoot";
    in.groundAttacker = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Fly) << "a ground mob: keep flying the leg down";
    in.groundAttacker = false;
    in.phase = Phase::Land;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Fly) << "a picket does not hold a landing";

    // Landed under fire: off now, without waiting for the Land phase.
    in = Flying();
    in.baseInCombat = true;
    in.groundAttacker = true;
    in.legArrived = true;
    in.landed = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount);
    in.isTank = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount) << "the tank too";
    in.groundAttacker = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount) << "a picket over a landed drake";

    // ...but never under a mounted master, and never in the air.
    in.masterMounted = true;
    EXPECT_NE(DcOculusRider::Decide(in).action, RiderAction::Dismount);
    in.masterMounted = false;
    in.landed = false;
    EXPECT_NE(DcOculusRider::Decide(in).action, RiderAction::Dismount);

    // The driver: a landed tank holds for stragglers only while nobody is fighting.
    DcOculusDriver::Inputs d = MountedOnTheRing();
    d.tankIsland = SITE_R2C;
    d.tankLanded = true;
    d.landed = 2;
    EXPECT_EQ(DcOculusDriver::Decide(d).state, State::LandWait);
    d.anyEngaged = true;
    DcOculusDriver::Verdict const v = DcOculusDriver::Decide(d);
    EXPECT_EQ(v.state, State::Land);
    EXPECT_EQ(v.phase, Phase::Land);
}

// Live, first run: every bot was on its own mount when it used the essence, the
// Call was refused, and no drake ever came. The rider steps off first.
TEST(Oculus, RiderStepsOffItsOwnMountBeforeTheCall)
{
    DcOculusRider::Inputs in = Flying();
    in.phase = Phase::Muster;
    in.onVehicle = false;
    in.onOcDrake = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = true;
    in.onMount = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::LeaveMount);

    in.onMount = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Mount) << "the cast on the next tick";

    // Back on a mount inside the settle: off again, or the saddle cast is lost too.
    in.mountIssuedMs = NOW - 1000;
    in.onMount = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::LeaveMount);
    in.onMount = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::SettleHold);

    // Walking to a giver mounted is fine; only the cast needs the bot on foot.
    in = Flying();
    in.phase = Phase::Muster;
    in.onVehicle = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = false;
    in.giverPresent = true;
    in.giverFlagged = true;
    in.giverDist = 20.0f;
    in.onMount = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::WalkToGiver);

    // Nothing to board: a bot's own mount is none of this rung's business.
    in.phase = Phase::Idle;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
}

// Live, first flight: the party's playerbots master rode a drake too, and stock
// MountingDrakeMultiplier zeroes every action on a bot that is on foot while its
// master is in a saddle. The master mounts last and steps off first.
TEST(Oculus, AMasterMountsLastAndStepsOffFirst)
{
    // The master, essence in hand, two of its four members still on foot.
    DcOculusRider::Inputs in = Flying();
    in.phase = Phase::Muster;
    in.onVehicle = false;
    in.onOcDrake = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = true;
    in.mastersOthers = true;
    in.othersAlive = 4;
    in.othersMounted = 2;
    DcOculusRider::Verdict v = DcOculusRider::Decide(in);
    EXPECT_EQ(v.action, RiderAction::None) << "mounting now freezes the two on foot";
    EXPECT_EQ(v.masterWaitSinceMs, NOW);

    in.othersMounted = 4;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Mount);

    // ...but not for ever.
    in.othersMounted = 3;
    in.masterWaitSinceMs = NOW - MASTER_MOUNT_WAIT_MS;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Mount);

    // A member landed under a master still in the saddle waits for it.
    in = Flying();
    in.phase = Phase::Land;
    in.legArrived = true;
    in.landed = true;
    in.masterMounted = true;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::None);
    in.masterMounted = false;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Dismount);

    // A stalled drake on the pad waits for the master the same way.
    in = Flying();
    in.phase = Phase::Land;
    in.legStalled = true;
    in.stallReissued = true;
    in.landed = true;
    in.masterMounted = true;
    EXPECT_NE(DcOculusRider::Decide(in).action, RiderAction::Dismount);

    // No master in the party (a `.dc test` GM off the map): nothing changes.
    in = Flying();
    in.phase = Phase::Muster;
    in.onVehicle = false;
    in.island = SITE_R1_GIVERS;
    in.hasEssence = true;
    in.othersAlive = 4;
    in.othersMounted = 0;
    EXPECT_EQ(DcOculusRider::Decide(in).action, RiderAction::Mount);
}

TEST(Oculus, TheRiderRungIsOneRungAboveEverythingItFliesOver)
{
    EXPECT_GT(DcRel::OcRider, DcRel::EventDueCombat);
    EXPECT_GT(DcRel::OcRider, DcRel::HazardVacate);
    EXPECT_LT(DcRel::OcRider, DcRel::BreakStuckCombat);
}
