/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Halls of Reflection (map 668).
//
// Three halves, and the split mirrors the module's:
//
//   1. THE WAVE KERNEL (DcHorWaves::Decide) — the whole of the altar driver's
//      per-tick reasoning, exercised without a map, a creature or an instance.
//      Most of what it decides is when to REFUSE, because for the first two
//      thirds of this dungeon there is nothing to pull and DC's ordinary
//      pipeline, left alone, walks the tank onto an invisible immune boss.
//
//   2. THE ESCAPE KERNEL (DcHorEscape::Decide) — the same for the second half,
//      where the encounter is decided entirely by POSITION. The Lich King walks a
//      fixed path at 1.445 yd/s and never pauses; a party that is ahead of the
//      leader survives it and a party that is behind him takes 10 000 damage plus
//      a knockback that puts them further behind.
//
//   3. THE AUTHORED DATA — the roster row that makes the escape exist as an
//      encounter at all, the five event rows and their flags, the instance-data
//      slots hand-copied out of halls_of_reflection.h, the registry rows, and the
//      geometry invariants cheap enough to assert without a navmesh (the
//      route-probe suite owns the ones that are not).
//
// THE MOST IMPORTANT ASSERTIONS IN THIS FILE are the instance-data slot pins.
// `enum Data` in halls_of_reflection.h is ONE enum holding three unrelated key
// spaces — boss-state indices, GetData/SetData slots and DoAction ids — with
// MAX_ENCOUNTER sitting in the middle of it at 3. Every driver decision on this
// map is a read through one of those numbers, and a wrong one does not fail
// loudly: it reads 0 for ever, which looks exactly like "the intro has not
// started" or "the wave count is zero".

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/FightInPlaceRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHorEscapeDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHorWaveDecision.h"

using namespace DcHallsOfReflection;

namespace
{
    // --- the wave kernel's baseline ---------------------------------------
    //
    // Mid-dungeon with every probe healthy: the intro is done, wave 3 is running,
    // nothing is currently armed and the tank is on the camp. Each test then
    // breaks exactly one thing.
    DcHorWaves::Inputs WaveBase()
    {
        DcHorWaves::Inputs in;
        in.introDone = true;
        in.falricDone = false;
        in.marwynDone = false;
        in.waveNumber = 3;
        in.mobsArmed = 0;
        in.bossAttackable = false;
        in.bossVisible = false;
        in.partyInCombat = false;
        in.distToCamp = 1.0f;
        in.campLeash = CAMP_LEASH;
        in.living = 5;
        in.nearCenter = 5;
        in.furthestFromCenter = 20.0f;
        in.nowMs = 1'000'000;
        in.state = static_cast<uint8>(DcHorWaves::State::Camp);
        in.stateSinceMs = 1'000'000 - 30'000;
        in.restartLogged = false;
        return in;
    }

    // --- the escape kernel's baseline -------------------------------------
    //
    // At wall 2, on the stand point, wall shut, no adds, the Lich King a
    // comfortable distance behind and ahead of nobody.
    DcHorEscape::Inputs EscapeBase()
    {
        DcHorEscape::Inputs in;
        in.active = true;
        in.leaderAlive = true;
        in.leaderHasHarvestSoul = false;
        in.leaderStop = 2;
        in.wallOpen = false;
        in.lkHasWinter = true;
        in.distToLk = 40.0f;
        in.sumMinusLkSum = -60.0f;   // well ahead of him
        in.lkToLeaderDist = 60.0f;
        in.distToStand = 1.0f;
        in.standLeash = STAND_LEASH;
        in.standLeaveLeash = STAND_LEAVE_LEASH;
        in.addsAlive = 0;
        in.addsTargetingMe = 0;
        in.partyInCombat = false;
        // On the stand point for wall 2, so the wall standoff reads clear.
        in.standWallDist = 18.8f;
        in.wallDist = 18.8f;
        in.wallStandoffSlack = WALL_STANDOFF_SLACK;
        in.wallStandoffClear = WALL_STANDOFF_CLEAR;
        // No loose summon and no idle clock running: every pickup test has to
        // say so itself, so the states that existed before this one cannot be
        // silently re-routed through it.
        in.looseAddDist = 0.0f;
        in.engageReach = ENGAGE_REACH_YD;
        in.engageMelee = ENGAGE_MELEE_YD;
        in.engageIdleMs = ENGAGE_IDLE_MS;
        in.idleSinceMs = 0;
        // Has NOT yet made this stop, so an off-stand test is a leg unless it
        // says otherwise. The baseline itself sits on the point, which sets the
        // latch inside Decide -- that is the arrival, not a carried-in state.
        in.reachedStand = false;
        in.pressureDist = LK_PRESSURE_DIST;
        in.behindSum = LK_BEHIND_SUM;
        in.nowMs = 1'000'000;
        in.state = static_cast<uint8>(DcHorEscape::State::Hold);
        in.stateSinceMs = 1'000'000 - 5'000;
        in.stallStop = 2;
        in.stallReported = false;
        in.stallDist = LK_STALL_DIST;
        in.stallMs = ESCAPE_STALL_MS;
        in.stopHoldSinceMs = 0;
        // Already settled at this stop, so the threat-pickup gate has expired and
        // a test that wants it has to say so.
        in.targetStopSinceMs = 1'000'000 - 60'000;
        in.grabThreatMs = GRAB_THREAT_MS;
        return in;
    }

    // --- the FOLLOWER kernel's baseline -----------------------------------
    //
    // A dps standing on the party's stand point at wall 2, out of his ring and
    // ahead of him on the scalar, with the advance latch clear. Nothing to do.
    DcHorEscape::FollowInputs FollowBase()
    {
        DcHorEscape::FollowInputs in;
        in.active = true;
        in.lkHasWinter = true;
        in.distToLk = 40.0f;
        in.sumMinusLkSum = -60.0f;
        in.pressureDist = LK_PRESSURE_DIST;
        in.behindSum = LK_BEHIND_SUM;
        in.distToStand = 1.0f;
        in.standLeash = STAND_LEASH;
        in.standLeaveLeash = STAND_LEAVE_LEASH;
        in.advancing = false;
        return in;
    }

    // Both kernels' shared contract: a verdict either CLAIMS the tick (by moving
    // or by holding) or YIELDS it, never both and never neither. Getting this
    // backwards is not a cosmetic bug — a driver that claims every tick starves
    // the combat engine outright (the tank never swings), and one that yields
    // while it should be steering hands the leg to DcRel::Advance, which on this
    // map walks the party at an invisible immune boss.
    void ExpectClaimsOrYields(DcHorWaves::Verdict const& v, char const* what)
    {
        int const claims = (v.walkToCamp ? 1 : 0) + (v.holdStill ? 1 : 0);
        if (v.yieldTick)
            EXPECT_EQ(claims, 0) << what << ": yields the tick AND claims it";
        else
            EXPECT_EQ(claims, 1) << what << ": neither yields the tick nor claims it";
    }

    void ExpectClaimsOrYields(DcHorEscape::Verdict const& v, char const* what)
    {
        int const claims = (v.travel ? 1 : 0) + (v.holdStill ? 1 : 0);
        if (v.yieldTick)
            EXPECT_EQ(claims, 0) << what << ": yields the tick AND claims it";
        else
            EXPECT_EQ(claims, 1) << what << ": neither yields the tick nor claims it";
    }
}

// =========================================================================
// 1. the wave kernel
// =========================================================================

// The window is EXACTLY "the intro has finished and Marwyn is not dead". Outside
// it the driver must be completely inert, because outside it the ordinary clear
// is doing the right thing — walking to the leader before, and to the Frostsworn
// General after.
TEST(DcHorWavesTest, TheWindowIsTheIntroFlagUntilMarwynDies)
{
    {
        DcHorWaves::Inputs in = WaveBase();
        in.introDone = false;
        DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
        EXPECT_EQ(v.state, DcHorWaves::State::Complete);
        EXPECT_TRUE(v.complete);
        EXPECT_TRUE(v.yieldTick);
        ExpectClaimsOrYields(v, "before the intro");
    }
    {
        DcHorWaves::Inputs in = WaveBase();
        in.marwynDone = true;
        // ...even with a wave apparently still live, which is the state the tick
        // after his death looks like.
        in.mobsArmed = 3;
        DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
        EXPECT_EQ(v.state, DcHorWaves::State::Complete);
        EXPECT_TRUE(v.complete);
        ExpectClaimsOrYields(v, "after Marwyn");
    }
}

// Completing CLEARS the stored block. The event is Repeatable, so a party that
// re-enters the window holding a spent restart latch would replay the whole first
// half of the dungeon without ever naming the wipe that caused it.
TEST(DcHorWavesTest, CompletionResetsTheStoredStateAndTheRestartLatch)
{
    DcHorWaves::Inputs in = WaveBase();
    in.marwynDone = true;
    in.state = static_cast<uint8>(DcHorWaves::State::Restart);
    in.stateSinceMs = 12345;
    in.restartLogged = true;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.storeState, static_cast<uint8>(DcHorWaves::State::Complete));
    EXPECT_EQ(v.stateSinceMs, 0u);
    EXPECT_FALSE(v.restartLogged);
}

// AN ARMED WAVE MOB YIELDS THE TICK, and this is the single most load-bearing
// branch in the kernel. Everything on this map comes to the party — every
// activation ends in SetInCombatWithZone() plus an AttackStart on the farthest
// player — so there is never anything to walk at, and the driver sits above the
// stock combat movers. A tick claimed here is a tick the tank does not swing,
// across ten waves.
TEST(DcHorWavesTest, AnArmedWaveMobYieldsTheTickToTheCombatEngine)
{
    DcHorWaves::Inputs in = WaveBase();
    in.mobsArmed = 4;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Fight);
    EXPECT_TRUE(v.yieldTick);
    EXPECT_FALSE(v.walkToCamp);
    ExpectClaimsOrYields(v, "a wave is up");
}

TEST(DcHorWavesTest, AnAttackableBossYieldsTheTickEvenWithNoTrashLeft)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 5;
    in.mobsArmed = 0;
    in.bossVisible = true;
    in.bossAttackable = true;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Fight);
    EXPECT_TRUE(v.yieldTick);
}

// THE EIGHT-SECOND ARMING WINDOW. On waves 5 and 10 the boss yells and becomes
// visible, and only 8 seconds later does SetImmuneToPC(false). A driver that
// yielded there would hand the tick to a combat engine with nothing to hit — and,
// worse, to the DcRel::AtBoss rung, which would walk the tank onto a boss that
// cannot be touched. Hold instead.
TEST(DcHorWavesTest, AVisibleButImmuneBossOnABossWaveHoldsRatherThanYields)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 5;
    in.bossVisible = true;
    in.bossAttackable = false;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::BossArm);
    EXPECT_FALSE(v.yieldTick);
    EXPECT_TRUE(v.holdStill);
    ExpectClaimsOrYields(v, "the boss has yelled but is immune");
}

// ...and on a TRASH wave the same "visible but immune" reading is just the
// pre-spawned boss standing there, which it does from t+186s of the intro
// onwards. That must not be reported as an arming window.
TEST(DcHorWavesTest, AVisibleImmuneBossOnATrashWaveIsOrdinaryCamping)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 3;
    in.bossVisible = true;
    in.bossAttackable = false;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Camp);
}

// The 60-second rest after Falric is the ONLY breathing space the instance
// gives. It is named so the log can tell it apart from an ordinary inter-wave
// gap, and it does the same thing: hold the camp.
TEST(DcHorWavesTest, TheRestAfterFalricIsNamedAndStillHoldsTheCamp)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 5;
    in.falricDone = true;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Rest);
    EXPECT_TRUE(v.holdStill);
    EXPECT_FALSE(v.yieldTick);
}

// THE WIPE. GetData(DATA_WAVE_NUMBER) is NOT monotonic — HandleWaveWipe puts it
// back to 0 — so "zero with the intro flag set" is not "the waves have not begun",
// it is "the event just wiped and every dead mob has respawned". Nothing else on
// the map distinguishes that from a healthy gap between waves, which is why the
// state exists and why it reports.
TEST(DcHorWavesTest, WaveZeroWithTheIntroDoneIsAWipeAndIsReportedOnce)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 0;
    in.state = static_cast<uint8>(DcHorWaves::State::Fight);  // we were fighting a tick ago

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Restart);
    EXPECT_TRUE(v.reportRestart);
    EXPECT_TRUE(v.restartLogged);
    EXPECT_TRUE(v.holdStill) << "the camp is inside the instance's own 40yd restart radius, "
                                "so holding it IS the restart";

    // Once, not once per tick: the restart can last a minute while the corpses
    // walk back.
    DcHorWaves::Inputs again = in;
    again.state = v.storeState;
    again.restartLogged = v.restartLogged;
    again.nowMs += 5'000;
    DcHorWaves::Verdict const v2 = DcHorWaves::Decide(again);
    EXPECT_EQ(v2.state, DcHorWaves::State::Restart);
    EXPECT_FALSE(v2.reportRestart);
}

// ...and a SECOND wipe later in the run is reported again. The latch is scoped to
// one episode, released by any state change, because a run that trips the leash
// twice has two things to tell the human and not one.
TEST(DcHorWavesTest, ASecondWipeIsReportedAgainAfterTheEventRestarts)
{
    DcHorWaves::Inputs in = WaveBase();
    in.waveNumber = 0;
    in.state = static_cast<uint8>(DcHorWaves::State::Restart);
    in.restartLogged = true;

    // The instance re-armed: wave 1 is running again.
    in.waveNumber = 1;
    in.mobsArmed = 3;
    DcHorWaves::Verdict const running = DcHorWaves::Decide(in);
    ASSERT_EQ(running.state, DcHorWaves::State::Fight);
    EXPECT_FALSE(running.restartLogged) << "a state change re-arms the wipe report";

    // ...and wipes again.
    DcHorWaves::Inputs second = in;
    second.state = running.storeState;
    second.restartLogged = running.restartLogged;
    second.waveNumber = 0;
    second.mobsArmed = 0;
    DcHorWaves::Verdict const v = DcHorWaves::Decide(second);
    EXPECT_EQ(v.state, DcHorWaves::State::Restart);
    EXPECT_TRUE(v.reportRestart);
}

TEST(DcHorWavesTest, ATankOffTheCampIsWalkedBackAndAClaimIsMadeEitherWay)
{
    {
        DcHorWaves::Inputs in = WaveBase();
        in.distToCamp = CAMP_LEASH + 20.0f;
        DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
        EXPECT_EQ(v.state, DcHorWaves::State::Camp);
        EXPECT_TRUE(v.walkToCamp);
        EXPECT_FALSE(v.yieldTick);
        ExpectClaimsOrYields(v, "off the camp");
    }
    {
        DcHorWaves::Inputs in = WaveBase();
        in.distToCamp = CAMP_LEASH - 0.5f;
        DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
        EXPECT_TRUE(v.holdStill);
        EXPECT_FALSE(v.walkToCamp);
        ExpectClaimsOrYields(v, "on the camp");
    }
}

// A FIGHT NEVER WALKS THE TANK BACK, even when it has been pushed a long way off
// the camp. The wave is on the party; walking away from it mid-fight would drop
// threat and drag the pack across the chamber.
TEST(DcHorWavesTest, TheDriverNeverWalksTheTankAwayFromALiveWave)
{
    DcHorWaves::Inputs in = WaveBase();
    in.mobsArmed = 2;
    in.distToCamp = 45.0f;

    DcHorWaves::Verdict const v = DcHorWaves::Decide(in);
    EXPECT_EQ(v.state, DcHorWaves::State::Fight);
    EXPECT_FALSE(v.walkToCamp);
    EXPECT_TRUE(v.yieldTick);
}

// The state clock re-stamps only on a real transition, so a log that prints one
// line per state change prints one line per state change.
TEST(DcHorWavesTest, TheStateClockRestampsOnlyOnATransition)
{
    DcHorWaves::Inputs in = WaveBase();
    in.state = static_cast<uint8>(DcHorWaves::State::Camp);
    in.stateSinceMs = 900'000;

    DcHorWaves::Verdict const same = DcHorWaves::Decide(in);
    ASSERT_EQ(same.state, DcHorWaves::State::Camp);
    EXPECT_EQ(same.stateSinceMs, 900'000u) << "an unchanged state must keep its clock";

    in.mobsArmed = 1;
    DcHorWaves::Verdict const changed = DcHorWaves::Decide(in);
    ASSERT_EQ(changed.state, DcHorWaves::State::Fight);
    EXPECT_EQ(changed.stateSinceMs, in.nowMs);
}

// =========================================================================
// 2. the escape kernel
// =========================================================================

TEST(DcHorEscapeTest, TheWindowIsTheBossStateAndTheLeaderBeingAlive)
{
    {
        DcHorEscape::Inputs in = EscapeBase();
        in.active = false;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Done);
        EXPECT_TRUE(v.complete);
        EXPECT_TRUE(v.yieldTick);
        ExpectClaimsOrYields(v, "not running");
    }
    {
        // A dead leader is a run wipe — the walls open only for her channel and
        // the boss state only flips when he reaches WP17 with four down. Stop
        // claiming ticks the party could spend surviving.
        DcHorEscape::Inputs in = EscapeBase();
        in.leaderAlive = false;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Done);
        EXPECT_TRUE(v.yieldTick);
    }
}

// HARVEST SOUL IS THE END. summonsCount has been set to 255 so no wall can ever
// open again and Fury of Frostmourne lands in three seconds. Say so once and get
// out of the way.
TEST(DcHorEscapeTest, HarvestSoulOnTheLeaderIsDoomedAndIsReportedExactlyOnce)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.leaderHasHarvestSoul = true;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Doomed);
    EXPECT_TRUE(v.reportDoomed);
    EXPECT_TRUE(v.yieldTick);
    ExpectClaimsOrYields(v, "doomed");

    DcHorEscape::Inputs again = in;
    again.state = v.storeState;
    again.nowMs += 1'000;
    EXPECT_FALSE(DcHorEscape::Decide(again).reportDoomed);
}

// ...and it outranks even a live wall fight. There is no positioning that
// survives Fury of Frostmourne, so nothing is gained by pretending otherwise.
TEST(DcHorEscapeTest, DoomedOutranksEverythingElse)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.leaderHasHarvestSoul = true;
    in.addsAlive = 6;
    in.distToLk = 4.0f;
    EXPECT_EQ(DcHorEscape::Decide(in).state, DcHorEscape::State::Doomed);
}

// PRESSURE OVERRIDES FIGHT, and this is the escape's equivalent of the wave
// kernel's yield: getting it wrong costs more per second than any add deals.
// Inside the ring it is 7068 +/- 863 frost every second; behind him it is 10 000
// plus a knockback that makes the next check worse.
TEST(DcHorEscapeTest, StandingInHisRingOverridesAFightAndStepsForward)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 5;
    in.partyInCombat = true;
    in.distToLk = LK_PRESSURE_DIST - 1.0f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Pressure);
    EXPECT_TRUE(v.travel) << "the answer is always FORWARD to the stand point, never a "
                             "radial retreat — away from him, for a bot behind him, is "
                             "further behind";
    EXPECT_FALSE(v.yieldTick);
    ExpectClaimsOrYields(v, "inside the ring");
}

TEST(DcHorEscapeTest, FallingBehindHimOnTheEncountersOwnScalarAlsoStepsForward)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.distToLk = 40.0f;             // nowhere near the ring
    in.sumMinusLkSum = LK_BEHIND_SUM + 0.5f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Pressure);
    EXPECT_TRUE(v.travel);
}

// The driver corrects at a fraction of the line the core actually zaps at (20),
// because the rule's own knockback is what makes it self-reinforcing.
TEST(DcHorEscapeTest, ThePressureThresholdIsWellInsideTheCoresOwnZapLine)
{
    EXPECT_LT(LK_BEHIND_SUM, 20.0f / 2.0f)
        << "the zap fires at (p.x - lk.x) + (p.y - lk.y) > 20 and its knockback throws the "
           "victim further behind; correcting anywhere near the line is correcting too late";
    EXPECT_GT(LK_PRESSURE_DIST, 10.0f)
        << "Remorseless Winter's pulse radius is 10yd — a threshold at or below it corrects "
           "only bots that are already taking 7000 frost a second";
}

// BOTH RULES ARE GATED ON REMORSELESS WINTER, because that is what gates the
// encounter's own. Before he casts it (~16s) and after the fourth wall removes
// it, standing near him is merely pointless.
TEST(DcHorEscapeTest, NeitherPressureRuleFiresWithoutRemorselessWinter)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.lkHasWinter = false;
    in.distToLk = 2.0f;
    in.sumMinusLkSum = 50.0f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_NE(v.state, DcHorEscape::State::Pressure);
}

TEST(DcHorEscapeTest, ThePartyAlwaysStandsAheadOfTheLeaderAndTheStopIsClamped)
{
    // She has not left WP0 yet; the party is already walking to stand 1.
    EXPECT_EQ(DcHorEscape::TargetStopFor(0), 1);
    EXPECT_EQ(DcHorEscape::TargetStopFor(1), 1);
    EXPECT_EQ(DcHorEscape::TargetStopFor(4), 4);
    // Stop 5 is WP18, where the wall geometry is over.
    EXPECT_EQ(DcHorEscape::TargetStopFor(5), 5);
    EXPECT_EQ(DcHorEscape::TargetStopFor(9), 5);
}

TEST(DcHorEscapeTest, ThePreludeAndTheAdvanceBothTravelAndAreNamedApart)
{
    {
        DcHorEscape::Inputs in = EscapeBase();
        in.leaderStop = 0;
        in.distToStand = 120.0f;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Prelude);
        EXPECT_EQ(v.targetStop, 1);
        EXPECT_TRUE(v.travel);
    }
    {
        DcHorEscape::Inputs in = EscapeBase();
        in.leaderStop = 3;
        in.wallOpen = true;
        in.distToStand = 90.0f;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Advance);
        EXPECT_EQ(v.targetStop, 3);
        EXPECT_TRUE(v.travel);
    }
}

// ADVANCE FIRES ON THE LEADER MOVING ON, NOT ON THE ADDS BEING DEAD, and that is
// deliberate: on heroic the last abomination of wall 4 (1.17M HP in that batch)
// can still be alive when the wall opens, and waiting for it would spend margin
// the party does not have. The next batch spawns at him 7.5 seconds after the
// wall opens and runs to the NEW stop regardless.
TEST(DcHorEscapeTest, TheAdvanceDoesNotWaitForTheLastAddToDie)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.leaderStop = 3;
    in.distToStand = 90.0f;
    in.addsAlive = 2;
    in.addsTargetingMe = 2;  // ...and they are on the tank, so they come along
    in.partyInCombat = true;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Advance);
    EXPECT_TRUE(v.travel);
}

// ...BUT IT DOES WAIT TO TAKE THE BATCH, briefly. The rule above is about the
// adds being ALIVE; this one is about them being LOOSE. On tp-20260907-221612-1
// the driver ran the tank 88yd to the next wall while eight summons were up and
// on the group — it arrived 153yd ahead of the Lich King, which is where they
// spawn, and the three non-tanks died to a batch nothing was holding.
//
// The summons chase whoever has threat, so a tank that leaves WITH them drags
// them to the wall it is going to anyway. The gate is only for picking them up.
TEST(DcHorEscapeTest, ALooseBatchIsTakenBeforeTheTankWalksToTheNextWall)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.leaderStop = 3;         // she has moved on: stand 3 is 90yd away
    in.distToStand = 90.0f;
    in.wallOpen = true;
    in.addsAlive = 8;
    in.addsTargetingMe = 0;    // ...and not one of them is on the tank
    in.partyInCombat = true;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Threat);
    EXPECT_TRUE(v.yieldTick) << "it has to swing to take them, not walk";
    EXPECT_FALSE(v.travel);
    ExpectClaimsOrYields(v, "the batch is loose and the leader has moved on");
    EXPECT_EQ(v.targetStopSinceMs, in.nowMs) << "the gate's clock starts at the stop change";
}

// AND THE GATE IS BOUNDED, because a Risen Witch Doctor casting from 20yd may
// never melee anybody: the tank must not be able to stand at a cleared wall for
// ever waiting for threat it cannot get. Past GRAB_THREAT_MS it walks regardless.
TEST(DcHorEscapeTest, TheThreatPickupGateExpiresAndTheTankWalksAnyway)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.leaderStop = 3;
    in.distToStand = 90.0f;
    in.wallOpen = true;
    in.addsAlive = 8;
    in.addsTargetingMe = 0;
    in.partyInCombat = true;
    in.stallStop = 3;  // already at this stop, so the change is not re-detected
    in.targetStopSinceMs = in.nowMs - GRAB_THREAT_MS - 1;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Advance);
    EXPECT_TRUE(v.travel);
}

// THE SCHMITT TRIGGER. With one radius the tank flipped Advance <-> Fight every
// one to two seconds for the whole of a wall fight — fifty-seven transitions in
// one run of tp-20260907-221612-1, distToStand alternating 6.7 and 5.0 — because
// each Advance re-issued a spline that cancelled the melee approach the combat
// engine had just laid over it. Arriving costs STAND_LEASH; leaving costs more.
TEST(DcHorEscapeTest, HoldingGroundDoesNotFlipToAdvanceOnASingleChaseStep)
{
    ASSERT_GT(STAND_LEAVE_LEASH, STAND_LEASH) << "the two radii ARE the hysteresis";

    // The exact drift that thrashed: just outside the arrive radius, well inside
    // the leave radius, while already holding ground.
    DcHorEscape::Inputs in = EscapeBase();
    in.distToStand = STAND_LEASH + 0.7f;
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;
    in.state = static_cast<uint8>(DcHorEscape::State::Fight);

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Fight) << "a chase step is not a departure";
    EXPECT_FALSE(v.travel);

    // Past the leave radius it is a real departure and the driver steers again.
    DcHorEscape::Inputs gone = in;
    gone.distToStand = STAND_LEAVE_LEASH + 0.1f;
    EXPECT_EQ(DcHorEscape::Decide(gone).state, DcHorEscape::State::Advance);

    // ...and a bot that is NOT already holding ground still has to get inside the
    // narrow radius before it counts as arrived, or the hysteresis would let it
    // stop six yards short every time.
    DcHorEscape::Inputs arriving = in;
    arriving.state = static_cast<uint8>(DcHorEscape::State::Advance);
    EXPECT_EQ(DcHorEscape::Decide(arriving).state, DcHorEscape::State::Advance);
}

// ...AND THE HYSTERESIS HAS TO BE WIDER THAN A WITCH DOCTOR'S CASTING RANGE.
// Twelve yards stopped the boundary flutter and did not stop the thrash, because
// the thing MoveChase walks the tank at is a Risen Witch Doctor parked at 20yd
// on the Lich King's side of the party. On tp-20260907-232556-1 the tank cycled
// 0.1yd -> past 12 -> Advance -> back to 0.1 fifteen times per run at stand 3,
// peaking at 24.7yd; each Advance claimed the tick and re-plotted the spline, so
// half of every wall fight was spent not fighting and three summons outlived a
// hundred seconds of it.
TEST(DcHorEscapeTest, TheLeaveLeashClearsAWitchDoctorsCastingRange)
{
    // 20yd of cast range plus the stand offset the melee starts from. Anything
    // narrower re-arms Advance in the middle of the approach that has to land for
    // the wall to open at all.
    ASSERT_GE(STAND_LEAVE_LEASH, 25.0f)
        << "a 20yd caster on his side of the party is a walk, not a chase step";

    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;
    in.state = static_cast<uint8>(DcHorEscape::State::Fight);
    in.distToStand = 24.7f;  // the measured peak of the thrash

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Fight)
        << "the melee approach to a 20yd caster must not re-arm Advance";
    EXPECT_FALSE(v.travel);
    ExpectClaimsOrYields(v, "melee approach inside the leave leash");

    // A real leg is 110-180yd and is a departure however wide the leash gets.
    DcHorEscape::Inputs leg = in;
    leg.distToStand = 110.0f;
    EXPECT_EQ(DcHorEscape::Decide(leg).state, DcHorEscape::State::Advance);

    // And the wider radius must not swallow the wall standoff: Recenter fires
    // only while atStand, so widening that covers MORE of the forward drift.
    DcHorEscape::Inputs clipping = in;
    clipping.wallDist = in.standWallDist - WALL_STANDOFF_SLACK - 1.0f;
    EXPECT_EQ(DcHorEscape::Decide(clipping).state, DcHorEscape::State::Recenter);
}

// THE TWO GRID-SCAN RANGES. A grid searcher answers "nothing within R" and
// "nothing at all" with the same empty list, and on this map they are routinely
// different things: the legs between stand points run 110-180yd and every summon
// batch is cast AT the Lich King, at the far end of the leg being crossed.
TEST(DcHorEscapeTest, TheGridScansReachTheFarEndOfTheLongestLeg)
{
    // The longest stand-to-previous-stop is stand 4 -> stop 3. The census has to
    // reach it or it reads a live batch as zero for the whole transit -- which
    // mutes the stall watchdog (it needs addsAlive > 0) and makes batchLoose
    // false in the one window the Threat pickup exists for.
    float worstLeg = 0.0f;
    for (uint8 stop = 1; stop <= 4; ++stop)
    {
        HorPoint const stand = StandPointFor(stop);
        HorPoint const& prev = PATH_WAYPOINTS[WP_STOP[stop - 1]];
        float const dx = stand.x - prev.x, dy = stand.y - prev.y, dz = stand.z - prev.z;
        worstLeg = std::max(worstLeg, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    EXPECT_GT(ESCAPE_ADD_SCAN_YD, worstLeg)
        << "the add census must reach the Lich King the batch spawns on";

    // The wall read is the other half: an EMPTY scan may only be believed from
    // inside a range where the scan could have seen the gameobject. The GO is
    // matched within 20yd of the target, so the readable range has to leave that
    // much room inside the scan itself.
    EXPECT_LE(WALL_READ_RANGE + 20.0f, WALL_SCAN_YD)
        << "an empty scan is only 'open' where the scan could have seen the wall";
}

// THE WALL STANDOFF. The stand points are clear of the slab; the tank does not
// stay on them, because yielding the fight lets MoveChase walk it forward. Bots
// were logged 13.4yd from a SHUT wall's origin against a stand point 18.8yd out.
// Stepping back is also stepping toward the summons, which are all behind.
TEST(DcHorEscapeTest, DriftingForwardIntoAShutWallIsWalkedBack)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;
    in.wallDist = in.standWallDist - WALL_STANDOFF_SLACK - 1.0f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Recenter);
    EXPECT_TRUE(v.travel) << "back to the stand point, which is behind";
    ExpectClaimsOrYields(v, "drifted into the shut wall");

    // Inside the slack it is an ordinary wall fight and nothing is corrected.
    DcHorEscape::Inputs slack = in;
    slack.wallDist = in.standWallDist - WALL_STANDOFF_SLACK + 0.1f;
    EXPECT_EQ(DcHorEscape::Decide(slack).state, DcHorEscape::State::Fight);

    // An OPEN wall is a doorway, not an obstacle: the party walks through it.
    DcHorEscape::Inputs opened = in;
    opened.wallOpen = true;
    EXPECT_NE(DcHorEscape::Decide(opened).state, DcHorEscape::State::Recenter);
}

// THE RECENTER STEP HAS TO ACTUALLY LAND, and for a year it never did.
//
// Recenter fires while the tank is AT the stand point -- it is a forward drift of
// a few yards INSIDE the leash -- and the driver issued it as
// TravelTo(stand, STAND_LEASH). TravelTo returns without moving when the bot is
// already inside the leash it is handed, and 6yd is wider than every clipping
// episode there is. On tr-20260908-225156-15 the state fired 27 times, logged at
// 1.4yd and 4.3yd from the stand, and moved the tank nowhere.
TEST(DcHorEscapeTest, TheRecenterArrivalLeashIsTighterThanTheDriftItCorrects)
{
    EXPECT_LT(WALL_RECENTER_LEASH, WALL_STANDOFF_SLACK)
        << "a leash wider than the drift makes the whole correction a no-op";
    EXPECT_LT(WALL_RECENTER_LEASH, STAND_LEASH)
        << "STAND_LEASH is what made it a no-op; the step back needs its own";
}

// ...AND IT HAS TO BE ONE STEP, NOT TWENTY-SEVEN. Arming and clearing on the same
// number is a single threshold, so the state dropped out the moment the tank was
// a yard better off and MoveChase re-armed it two seconds later. It now HOLDS
// until the standoff is genuinely back.
TEST(DcHorEscapeTest, TheRecenterHoldsUntilTheStandoffIsActuallyCleared)
{
    ASSERT_LT(WALL_STANDOFF_CLEAR, WALL_STANDOFF_SLACK)
        << "clearing must be a STRICTER test than arming, or there is no hysteresis";

    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;

    // Part-way back: past the arming threshold, not yet at the clearing one.
    // From Fight this is an ordinary wall fight; from Recenter it is unfinished.
    float const partWayBack = in.standWallDist - WALL_STANDOFF_SLACK + 0.5f;
    ASSERT_LT(partWayBack, in.standWallDist - WALL_STANDOFF_CLEAR);

    DcHorEscape::Inputs fighting = in;
    fighting.wallDist = partWayBack;
    fighting.state = static_cast<uint8>(DcHorEscape::State::Fight);
    EXPECT_EQ(DcHorEscape::Decide(fighting).state, DcHorEscape::State::Fight)
        << "arming is still the slack -- a bot that never clipped is left alone";

    DcHorEscape::Inputs stepping = in;
    stepping.wallDist = partWayBack;
    stepping.state = static_cast<uint8>(DcHorEscape::State::Recenter);
    EXPECT_EQ(DcHorEscape::Decide(stepping).state, DcHorEscape::State::Recenter)
        << "the step back must hold to the clearing threshold, not drop out early";

    // All the way back onto the standoff: done.
    DcHorEscape::Inputs done = stepping;
    done.wallDist = in.standWallDist;
    EXPECT_NE(DcHorEscape::Decide(done).state, DcHorEscape::State::Recenter);
}

// =========================================================================
// THE LEASH IS A BAND, NOT A PARKING SPACE
// =========================================================================
//
// tr-20260908-225156-15, wall 3: Fight at 7.1yd -> chased a Risen Witch Doctor to
// 28.2 -> crossed STAND_LEAVE_LEASH -> travelled all the way home to 6 -> Fight
// -> out to 28.8 -> round again. Two full 30-to-6yd round trips in twenty
// seconds, each re-plotting a spline over the melee approach. The tank does not
// need to be on the point; it needs to be in the band.
TEST(DcHorEscapeTest, ADriftedTankReturnsToTheBandEdgeNotTheStandPoint)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;
    in.state = static_cast<uint8>(DcHorEscape::State::Fight);
    in.reachedStand = true;               // the party owns this stop already
    in.distToStand = STAND_LEAVE_LEASH + 1.0f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Regroup);
    EXPECT_TRUE(v.travel);
    EXPECT_TRUE(v.travelToEdge) << "the near edge of the band, never the centre";
    EXPECT_FALSE(v.travelToAdd);
    ExpectClaimsOrYields(v, "drifted off a stop the party already owns");
    EXPECT_TRUE(v.reachedStand) << "drifting out does not un-arrive";
}

// ...AND AN UNFINISHED LEG IS THE OPPOSITE CASE. 110-180yd with the party not yet
// at the wall is a crossing, and a crossing has to end ON the point -- the band
// edge is 24yd of nothing, a hundred yards short of where the batch will arrive.
TEST(DcHorEscapeTest, ALegThatHasNeverArrivedStillEndsOnTheStandPoint)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.reachedStand = false;
    in.distToStand = 110.0f;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Advance);
    EXPECT_TRUE(v.travel);
    EXPECT_FALSE(v.travelToEdge) << "a 110yd crossing must not stop 24yd short";
}

// A NEW STOP IS ALWAYS A LEG. Whatever the party had reached belongs to the wall
// it is leaving; carrying the latch across would turn the next 180yd crossing
// into a recall that stops 24yd from the point.
TEST(DcHorEscapeTest, ArrivalIsForgottenWhenTheLeaderMovesOn)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.reachedStand = true;
    in.distToStand = 140.0f;
    in.leaderStop = 3;      // she has moved on...
    in.stallStop = 2;       // ...and the latch still belongs to stop 2

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_FALSE(v.reachedStand) << "the new stop has never been reached";
    EXPECT_EQ(v.state, DcHorEscape::State::Advance);
    EXPECT_FALSE(v.travelToEdge);
    EXPECT_EQ(v.idleSinceMs, 0u) << "a stale idle arm must not cross a leg";
}

// THE RECALL MUST NOT RE-ARM ITSELF. It ends inside the WIDE leash by
// construction (STAND_EDGE_MARGIN inside it), so reading Regroup against the
// narrow one would leave atStand false at the very point it just walked to.
TEST(DcHorEscapeTest, TheRecallEndsInsideTheBandItWasRecalledTo)
{
    ASSERT_LT(STAND_EDGE_MARGIN, STAND_LEAVE_LEASH);
    ASSERT_GT(STAND_LEAVE_LEASH - STAND_EDGE_MARGIN, STAND_LEASH)
        << "the edge point has to sit OUTSIDE the narrow leash, or this is the "
           "run-all-the-way-home the whole change removes";

    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 3;
    in.partyInCombat = true;
    in.reachedStand = true;
    in.state = static_cast<uint8>(DcHorEscape::State::Regroup);
    in.distToStand = STAND_LEAVE_LEASH - STAND_EDGE_MARGIN;  // where it lands

    EXPECT_EQ(DcHorEscape::Decide(in).state, DcHorEscape::State::Fight)
        << "back in the band is back in the fight, not another recall";
}

// =========================================================================
// THE IDLE TANK
// =========================================================================
//
// tr-20260908-225156-15, wall 4, 23:04:41 to 23:06:08: distToStand pinned at
// exactly 1.0yd for EIGHTY-SEVEN SECONDS while nine summons fell to two without a
// swing from the tank. The engine trace is 448 ticks of "no actions executed",
// 399 melee prereq failures and every ability IMPOSSIBLE with result 67 (out of
// range) against a Risen Witch Doctor. Fight had yielded the tick and there was
// nothing left in the stock engine that closes twenty yards.
TEST(DcHorEscapeTest, ATankWithNothingOnItWalksAtTheLooseSummon)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 0;         // none of it is on me
    in.partyInCombat = true;
    in.looseAddDist = 20.0f;        // a witch doctor's cast range
    in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Engage);
    EXPECT_TRUE(v.travel);
    EXPECT_TRUE(v.travelToAdd);
    EXPECT_FALSE(v.travelToEdge);
    ExpectClaimsOrYields(v, "nothing on the tank and nothing in reach");
}

// ...BUT NOT ON THE GAP BETWEEN ONE SUMMON DYING AND THE NEXT ARRIVING, which is
// under a second and is not a reason to walk anywhere.
TEST(DcHorEscapeTest, ThePickupWaitsOutTheGapBetweenSummons)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 0;
    in.partyInCombat = true;
    in.looseAddDist = 20.0f;
    in.idleSinceMs = in.nowMs - (ENGAGE_IDLE_MS / 2);

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Fight) << "half the arm is not the arm";
    EXPECT_EQ(v.idleSinceMs, in.idleSinceMs) << "the clock has to keep running";

    // A cold clock STARTS here rather than firing here.
    DcHorEscape::Inputs cold = in;
    cold.idleSinceMs = 0;
    DcHorEscape::Verdict const started = DcHorEscape::Decide(cold);
    EXPECT_EQ(started.state, DcHorEscape::State::Fight);
    EXPECT_EQ(started.idleSinceMs, in.nowMs) << "first tick of idle stamps the clock";
}

// BOUNDED ON BOTH SIDES. Inside melee reach the rotation gets there unaided and
// steering would only tear down the approach; past ENGAGE_REACH_YD the summon is
// not a straggler, it is still running in from the Lich King two hundred yards
// back, and walking at it is walking at HIM.
TEST(DcHorEscapeTest, ThePickupIsBoundedAtBothEnds)
{
    ASSERT_LT(ENGAGE_MELEE_YD, ENGAGE_REACH_YD);
    EXPECT_LT(ENGAGE_REACH_YD, STAND_LEAVE_LEASH)
        << "a pickup begun on the stand point must not end outside the band and "
           "hand itself straight to the recall";
    EXPECT_GE(ENGAGE_REACH_YD, 20.0f)
        << "the witch doctor this exists for casts from 20yd";

    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 0;
    in.partyInCombat = true;
    in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;

    DcHorEscape::Inputs inMelee = in;
    inMelee.looseAddDist = ENGAGE_MELEE_YD - 0.5f;
    EXPECT_EQ(DcHorEscape::Decide(inMelee).state, DcHorEscape::State::Fight)
        << "the rotation reaches this one on its own";

    DcHorEscape::Inputs faraway = in;
    faraway.looseAddDist = ENGAGE_REACH_YD + 0.5f;
    EXPECT_EQ(DcHorEscape::Decide(faraway).state, DcHorEscape::State::Fight)
        << "that is an inbound summon, not a straggler";

    // And no candidate at all is the sentinel, not a zero-distance target.
    DcHorEscape::Inputs none = in;
    none.looseAddDist = 0.0f;
    EXPECT_EQ(DcHorEscape::Decide(none).state, DcHorEscape::State::Fight);
}

// A SUMMON THAT IS ON THE TANK IS THE TANK DOING ITS JOB. The clock resets, so a
// batch that lands mid-fight cannot leave a stale arm behind it.
TEST(DcHorEscapeTest, ASummonOnTheTankIsNeverAPickup)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 1;
    in.partyInCombat = true;
    in.looseAddDist = 20.0f;
    in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Fight);
    EXPECT_EQ(v.idleSinceMs, 0u) << "something is on me; the idle clock is dead";
}

// THE ORDER OF THE THREE NEW REASONS AGAINST THE OLD ONES. Pressure still outranks
// everything (7068 frost/s beats any add), the slab still outranks the pickup
// (walking at a summon must never be walking into the wall), and a leg still
// outranks both.
TEST(DcHorEscapeTest, ThePickupNeverOutranksPressureOrTheSlab)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 0;
    in.partyInCombat = true;
    in.looseAddDist = 20.0f;
    in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;
    ASSERT_EQ(DcHorEscape::Decide(in).state, DcHorEscape::State::Engage);

    DcHorEscape::Inputs pressured = in;
    pressured.distToLk = LK_PRESSURE_DIST - 1.0f;
    EXPECT_EQ(DcHorEscape::Decide(pressured).state, DcHorEscape::State::Pressure);

    DcHorEscape::Inputs clipping = in;
    clipping.wallDist = in.standWallDist - WALL_STANDOFF_SLACK - 1.0f;
    EXPECT_EQ(DcHorEscape::Decide(clipping).state, DcHorEscape::State::Recenter);

    DcHorEscape::Inputs leg = in;
    leg.distToStand = 110.0f;
    EXPECT_NE(DcHorEscape::Decide(leg).state, DcHorEscape::State::Engage);
}

// THE PICKUP MUST NOT TURN ITSELF INTO A RECALL. Engage deliberately walks AWAY
// from the stand point, up to ENGAGE_REACH_YD -- read against the narrow leash
// that would be "gone" on the very next tick.
TEST(DcHorEscapeTest, WalkingAtASummonIsNotReadAsLeavingTheStand)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 3;
    in.addsTargetingMe = 0;
    in.partyInCombat = true;
    in.looseAddDist = 20.0f;
    in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;
    in.reachedStand = true;
    in.state = static_cast<uint8>(DcHorEscape::State::Engage);
    in.distToStand = 20.0f;   // mid-pickup, outside STAND_LEASH, inside the band

    EXPECT_EQ(DcHorEscape::Decide(in).state, DcHorEscape::State::Engage)
        << "the pickup holds the wide leash like every other holding-ground state";
}

// EXACTLY ONE DESTINATION PER STEER, for every state the kernel can reach. The two
// flags are read by the glue to pick between three different points; both set at
// once, or either set without `travel`, is a bot walked somewhere nobody chose.
TEST(DcHorEscapeTest, ASteerNeverCarriesTwoDestinations)
{
    auto check = [](DcHorEscape::Verdict const& v, char const* what)
    {
        if (!v.travel)
        {
            EXPECT_FALSE(v.travelToEdge) << what << ": aimed without steering";
            EXPECT_FALSE(v.travelToAdd) << what << ": aimed without steering";
            return;
        }
        EXPECT_FALSE(v.travelToEdge && v.travelToAdd) << what << ": two destinations";
    };

    DcHorEscape::Inputs base = EscapeBase();
    base.addsAlive = 3;
    base.partyInCombat = true;

    for (uint8 leaderStop = 0; leaderStop <= 5; ++leaderStop)
        for (uint32 onMe : {0u, 3u})
            for (float dist : {0.5f, 20.0f, 110.0f})
                for (bool reached : {false, true})
                    for (bool clipped : {false, true})
                    {
                        DcHorEscape::Inputs in = base;
                        in.leaderStop = leaderStop;
                        in.addsTargetingMe = onMe;
                        in.distToStand = dist;
                        in.reachedStand = reached;
                        in.looseAddDist = onMe ? 0.0f : 20.0f;
                        in.idleSinceMs = in.nowMs - ENGAGE_IDLE_MS - 1;
                        if (clipped)
                            in.wallDist = in.standWallDist - WALL_STANDOFF_SLACK - 1.0f;
                        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
                        check(v, "escape sweep");
                        ExpectClaimsOrYields(v, "escape sweep");
                    }
}

// THE STALL WARN HAS TO BE REACHABLE. He closes on a stationary leader at
// 1.445 yd/s and catches her at 12.5yd, so the window between crossing
// LK_STALL_DIST and the catch is (LK_STALL_DIST - 12.5) / 1.445 seconds. The
// first pair (30yd / 20s) gave 12.1 seconds for a latch that needed 20 — ten
// runs stalled at a wall and produced zero STALLED lines between them.
TEST(DcHorEscapeTest, TheStallBudgetFitsInsideTheTimeHeTakesToCoverTheStallDistance)
{
    constexpr float kCatchDist = 12.5f;      // npc_hor_lich_kingAI's own catch range
    constexpr float kLkSpeed = 1.445f;       // yd/s under Remorseless Winter

    ASSERT_GT(LK_STALL_DIST, kCatchDist);
    float const windowS = (LK_STALL_DIST - kCatchDist) / kLkSpeed;
    float const budgetS = static_cast<float>(ESCAPE_STALL_MS) / 1000.0f;

    EXPECT_LT(budgetS, windowS)
        << "the latch needs " << budgetS << "s but only " << windowS
        << "s exist between LK_STALL_DIST and the catch — it can never fire";
    // ...and with enough left over that the line is a diagnosis rather than an
    // obituary: at least ten seconds of warning before he reaches her.
    EXPECT_GE(windowS - budgetS, 10.0f);
}

TEST(DcHorEscapeTest, AWallFightOnTheStandPointYieldsTheTick)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 7;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_EQ(v.state, DcHorEscape::State::Fight);
    EXPECT_TRUE(v.yieldTick);
    EXPECT_FALSE(v.travel);
    ExpectClaimsOrYields(v, "the wall's summons are on the party");
}

TEST(DcHorEscapeTest, AQuietStandPointHolds)
{
    DcHorEscape::Verdict const v = DcHorEscape::Decide(EscapeBase());
    EXPECT_EQ(v.state, DcHorEscape::State::Hold);
    EXPECT_TRUE(v.holdStill);
    ExpectClaimsOrYields(v, "holding the stand point");
}

TEST(DcHorEscapeTest, TheFinalRunTargetsTheEndOfThePathAndStopsWhenItArrives)
{
    {
        DcHorEscape::Inputs in = EscapeBase();
        in.leaderStop = 5;
        in.lkHasWinter = false;  // the fourth wall removed it
        in.distToStand = 130.0f;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Final);
        EXPECT_EQ(v.targetStop, 5);
        EXPECT_TRUE(v.travel);
    }
    {
        DcHorEscape::Inputs in = EscapeBase();
        in.leaderStop = 5;
        in.lkHasWinter = false;
        in.distToStand = 1.0f;
        DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
        EXPECT_EQ(v.state, DcHorEscape::State::Final);
        EXPECT_TRUE(v.holdStill);
    }
}

// THE STALL WATCHDOG. He is closing on her, the wall is still shut, and the adds
// that hold it are not dying. Unrecoverable from inside the driver — the known
// cause is a Risen Witch Doctor parked inside the 10yd ring, casting from 20yd at
// the rearmost bot, which melee cannot reach and the pressure rule keeps pulling
// them off — so the honest act is to name it once with the adds listed.
TEST(DcHorEscapeTest, TheStallIsReportedOncePerStopAndOnlyWhileTheWallIsShut)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 2;
    in.lkToLeaderDist = LK_STALL_DIST - 1.0f;

    // First tick inside the danger band: the clock starts, nothing is said yet.
    DcHorEscape::Verdict const first = DcHorEscape::Decide(in);
    EXPECT_FALSE(first.reportStall);
    EXPECT_EQ(first.stopHoldSinceMs, in.nowMs);

    // Past the budget: reported.
    DcHorEscape::Inputs late = in;
    late.state = first.storeState;
    late.stopHoldSinceMs = first.stopHoldSinceMs;
    late.stallStop = first.stallStop;
    late.stallReported = first.stallReported;
    late.nowMs = in.nowMs + ESCAPE_STALL_MS + 1;

    DcHorEscape::Verdict const warned = DcHorEscape::Decide(late);
    EXPECT_TRUE(warned.reportStall);
    EXPECT_TRUE(warned.stallReported);

    // ...once.
    DcHorEscape::Inputs again = late;
    again.state = warned.storeState;
    again.stopHoldSinceMs = warned.stopHoldSinceMs;
    again.stallStop = warned.stallStop;
    again.stallReported = warned.stallReported;
    again.nowMs += 5'000;
    EXPECT_FALSE(DcHorEscape::Decide(again).reportStall);
}

TEST(DcHorEscapeTest, TheStallLatchReArmsAtTheNextWall)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.addsAlive = 2;
    in.lkToLeaderDist = LK_STALL_DIST - 1.0f;
    in.stallStop = 2;
    in.stallReported = true;
    in.stopHoldSinceMs = in.nowMs - ESCAPE_STALL_MS - 1;

    // Same wall: still quiet.
    EXPECT_FALSE(DcHorEscape::Decide(in).reportStall);

    // The leader moved on. The latch belongs to the STOP, not to the state,
    // because the party legitimately cycles Hold -> Fight -> Hold several times at
    // one wall and a per-state latch would re-report on every cycle.
    DcHorEscape::Inputs next = in;
    next.leaderStop = 3;
    DcHorEscape::Verdict const v = DcHorEscape::Decide(next);
    EXPECT_EQ(v.stallStop, 3);
    EXPECT_FALSE(v.stallReported) << "the new wall gets its own budget";
    EXPECT_EQ(v.stopHoldSinceMs, next.nowMs) << "and its own clock";
}

TEST(DcHorEscapeTest, AnOpenWallNeverStalls)
{
    DcHorEscape::Inputs in = EscapeBase();
    in.wallOpen = true;
    in.addsAlive = 2;
    in.lkToLeaderDist = 5.0f;
    in.stopHoldSinceMs = in.nowMs - ESCAPE_STALL_MS - 1;

    DcHorEscape::Verdict const v = DcHorEscape::Decide(in);
    EXPECT_FALSE(v.reportStall);
    EXPECT_EQ(v.stopHoldSinceMs, 0u) << "the clock is dropped, not carried to the next wall";
}

// =========================================================================
// 3. the follower kernel — "come with the tank"
// =========================================================================

// The whole of tp-20260908-000109-1 in one assertion. The wall has opened, the
// leader is 136yd up the corridor at her next stop and the tank has gone after
// her; the Lich King is nowhere near this bot, so the PRESSURE half says nothing
// at all. Before DecideFollow existed that was the only half there was, and the
// dps stayed at the old wall until he walked into them.
TEST(DcHorFollowTest, AFollowerLeftBehindByAnOpenedWallTravelsEvenWithNoPressure)
{
    DcHorEscape::FollowInputs in = FollowBase();
    in.distToStand = 136.0f;
    in.distToLk = 90.0f;          // far outside the ring
    in.sumMinusLkSum = -120.0f;   // and comfortably ahead of him

    DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
    EXPECT_FALSE(v.pressured);
    EXPECT_TRUE(v.advancing);
    EXPECT_TRUE(v.travel);
}

// The advance arm is a SCHMITT TRIGGER, exactly as the driver's is, and this is
// the property that makes it work at all: a bot that armed at 40yd and disarmed
// at 39 would hand the tick straight back to MoveChase, which walks it toward
// the add it just left, which re-arms the rung — the oscillation Redas ran for
// twenty seconds in tr-20260908-000117-1 while covering no ground.
TEST(DcHorFollowTest, TheAdvanceLatchArmsWideAndClearsNarrow)
{
    DcHorEscape::FollowInputs in = FollowBase();

    // Inside the wide radius with the latch clear: nothing happens. A follower
    // fighting the batch a few yards off the stand point is not "left behind".
    in.distToStand = STAND_LEAVE_LEASH - 1.0f;
    EXPECT_FALSE(DcHorEscape::DecideFollow(in).advancing);
    EXPECT_FALSE(DcHorEscape::DecideFollow(in).travel);

    // Past it, it arms.
    in.distToStand = STAND_LEAVE_LEASH + 1.0f;
    DcHorEscape::FollowVerdict armed = DcHorEscape::DecideFollow(in);
    EXPECT_TRUE(armed.advancing);
    EXPECT_TRUE(armed.travel);

    // And STAYS armed all the way in, through the whole band the arm test would
    // have dropped it in.
    in.advancing = armed.advancing;
    for (float d : { STAND_LEAVE_LEASH - 1.0f, 20.0f, 12.0f, STAND_LEASH + 0.5f })
    {
        in.distToStand = d;
        DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
        EXPECT_TRUE(v.advancing) << "latch dropped at " << d << "yd";
        EXPECT_TRUE(v.travel) << "stopped travelling at " << d << "yd";
        in.advancing = v.advancing;
    }

    // Arrival clears it, and clears the travel with it.
    in.distToStand = STAND_LEASH - 0.1f;
    DcHorEscape::FollowVerdict const done = DcHorEscape::DecideFollow(in);
    EXPECT_FALSE(done.advancing);
    EXPECT_FALSE(done.travel);
}

// The original rung, unchanged: inside his ring, or behind him on the encounter's
// own scalar, is a move even when the party has not gone anywhere.
TEST(DcHorFollowTest, ThePressureHalfStillFiresOnItsOwn)
{
    {
        DcHorEscape::FollowInputs in = FollowBase();
        in.distToStand = 12.0f;   // inside the wide leash, so no advance
        in.distToLk = LK_PRESSURE_DIST - 1.0f;
        DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
        EXPECT_TRUE(v.pressured);
        EXPECT_FALSE(v.advancing);
        EXPECT_TRUE(v.travel);
    }
    {
        DcHorEscape::FollowInputs in = FollowBase();
        in.distToStand = 12.0f;
        in.sumMinusLkSum = LK_BEHIND_SUM + 1.0f;
        DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
        EXPECT_TRUE(v.pressured);
        EXPECT_TRUE(v.travel);
    }
}

// Remorseless Winter gates the PRESSURE half and nothing else. Both of the
// encounter's own rules hang off that aura, so without it standing near him is
// merely pointless — but the last 131yd to the gunship is run with the aura gone
// and the party still has to make it together.
TEST(DcHorFollowTest, WinterGatesThePressureHalfButNotTheAdvanceHalf)
{
    DcHorEscape::FollowInputs in = FollowBase();
    in.lkHasWinter = false;
    in.distToLk = 2.0f;               // standing on him
    in.sumMinusLkSum = 50.0f;         // and well behind him
    in.distToStand = 12.0f;
    EXPECT_FALSE(DcHorEscape::DecideFollow(in).travel);

    // ...and the run for WP18, on the same dead aura, still moves.
    in.distToStand = 131.0f;
    DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
    EXPECT_FALSE(v.pressured);
    EXPECT_TRUE(v.advancing);
    EXPECT_TRUE(v.travel);
}

// ALREADY THERE outranks both reasons. A bot on the stand point that still reads
// pressured has had the Lich King walk up to IT — the ordinary state of a wall
// that is taking a while — and re-issuing a move to where it is standing would
// only tear down the melee approach the combat engine is running. The answer to
// that situation is killing the wall's adds.
TEST(DcHorFollowTest, ABotOnTheStandPointNeverReIssuesAMoveToWhereItIs)
{
    DcHorEscape::FollowInputs in = FollowBase();
    in.distToStand = STAND_LEASH - 0.5f;
    in.distToLk = 4.0f;
    in.sumMinusLkSum = 30.0f;
    in.advancing = true;   // ...even arriving off a leg

    DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
    EXPECT_TRUE(v.pressured);
    EXPECT_FALSE(v.advancing) << "arrival must clear the latch";
    EXPECT_FALSE(v.travel);
}

// Outside the escape the rung is inert and the latch is dropped, so a bot that
// died mid-leg and was resurrected after the event does not resume walking at a
// stand point the encounter has finished with.
TEST(DcHorFollowTest, AnInactiveEscapeIsInertAndDropsTheLatch)
{
    DcHorEscape::FollowInputs in = FollowBase();
    in.active = false;
    in.advancing = true;
    in.distToStand = 200.0f;
    in.distToLk = 1.0f;

    DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
    EXPECT_FALSE(v.travel);
    EXPECT_FALSE(v.advancing);
    EXPECT_FALSE(v.pressured);
}

// The tank is steered by hook 35, which walks it to this same stand point behind
// its OWN re-issue floor. Arming the advance half there too would give one bot
// two independent 1500ms floors both re-plotting a spline at the same point —
// exactly the double-issue thrash STAND_LEAVE_LEASH was widened to end. The
// pressure half is left alone: it costs one extra correction at most and the
// driver's own Pressure state wants the same move.
TEST(DcHorFollowTest, TheDriverSteeredBotGetsThePressureHalfButNotTheAdvanceHalf)
{
    DcHorEscape::FollowInputs in = FollowBase();
    in.driverSteered = true;
    in.distToStand = 136.0f;
    in.distToLk = 90.0f;
    in.sumMinusLkSum = -120.0f;

    DcHorEscape::FollowVerdict const v = DcHorEscape::DecideFollow(in);
    EXPECT_FALSE(v.advancing);
    EXPECT_FALSE(v.travel);

    // ...and an already-armed latch is HELD clear, not merely left alone, so a
    // bot promoted to leader mid-leg does not carry one into the driver's lap.
    in.advancing = true;
    EXPECT_FALSE(DcHorEscape::DecideFollow(in).advancing);

    // The pressure half still fires on it.
    in.distToLk = LK_PRESSURE_DIST - 1.0f;
    DcHorEscape::FollowVerdict const pressed = DcHorEscape::DecideFollow(in);
    EXPECT_TRUE(pressed.pressured);
    EXPECT_TRUE(pressed.travel);
    EXPECT_FALSE(pressed.advancing);
}

// THE TWO HALVES OF THE ESCAPE MUST AGREE ABOUT WHERE THE PARTY IS GOING. The
// driver picks the tank's destination with TargetStopFor + StandPointFor off the
// leader's stop; the follower rung picks its own with the same two calls off the
// same leader. If they ever diverged the tank and its followers would hold ground
// fifty yards apart with a Lich King between them.
TEST(DcHorFollowTest, TheFollowerAndTheDriverResolveTheSameStandPoint)
{
    for (uint8 leaderStop = 0; leaderStop <= 6; ++leaderStop)
    {
        uint8 const target = DcHorEscape::TargetStopFor(leaderStop);
        HorPoint const driverStand = StandPointFor(target);

        // The follower's own derivation, as DcHorStayAheadAction::Probe does it.
        uint8 const followerTarget = DcHorEscape::TargetStopFor(leaderStop);
        HorPoint const followerStand = StandPointFor(followerTarget);

        EXPECT_EQ(followerTarget, target) << "leader at stop " << int(leaderStop);
        EXPECT_FLOAT_EQ(followerStand.x, driverStand.x);
        EXPECT_FLOAT_EQ(followerStand.y, driverStand.y);
        EXPECT_FLOAT_EQ(followerStand.z, driverStand.z);
    }
}

// The legs this rung has to cover are LONG — that is the whole reason the arm
// radius is not simply "off the stand point". Pinning them here means a change to
// the stand points that shortened a leg below the hysteresis band would trip red
// rather than silently turning the advance arm into a no-op.
TEST(DcHorFollowTest, EveryLegBetweenStandPointsIsWellOutsideTheArmRadius)
{
    auto legLength = [](HorPoint const& a, HorPoint const& b) {
        float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    for (uint8 stop = 1; stop <= 4; ++stop)
    {
        float const leg = legLength(StandPointFor(stop), StandPointFor(stop + 1));
        EXPECT_GT(leg, STAND_LEAVE_LEASH * 2.0f)
            << "the leg from stand " << int(stop) << " to " << int(stop + 1)
            << " is only " << leg << "yd — the advance arm would barely fire";
    }
}

// =========================================================================
// 4. the authored data
// =========================================================================

// THE INSTANCE-DATA SLOTS, hand-copied from halls_of_reflection.h. `enum Data`
// holds three unrelated key spaces in one sequence with MAX_ENCOUNTER at 3 in the
// middle of it, so a hand-counted slot is off by one and reads 0 for ever — which
// looks exactly like "the intro has not started".
TEST(DungeonEventHallsOfReflectionTest, TheInstanceDataSlotsMatchTheCoreEnum)
{
    EXPECT_EQ(DATA_FALRIC, 0u);
    EXPECT_EQ(DATA_MARWYN, 1u);
    EXPECT_EQ(DATA_LICH_KING, 2u);
    // MAX_ENCOUNTER occupies 3. DATA_INTRO is 4, DATA_FROSTSWORN_GENERAL 5,
    // DATA_BATTERED_HILT is an EXPLICIT `= 6`, DATA_LK_INTRO 7...
    EXPECT_EQ(DATA_BATTERED_HILT, 6u);
    EXPECT_EQ(DATA_WAVE_NUMBER, 8u);

    // AND THE TWO BOSSES' GetGuidData KEYS ARE THOSE SAME DATA SLOTS, not their
    // entries. instance_halls_of_reflection's creatureData files most of this map
    // under the entry ({ NPC_SYLVANAS_PART1, NPC_SYLVANAS_PART1 }) but Falric and
    // Marwyn under a DATA_ type, and LoadObjectData keys the guid store by TYPE —
    // so a lookup on 38112 returns ObjectGuid::Empty for ever, which reads as "no
    // boss is up" and would hold the party at the camp through both boss fights
    // instead of yielding the tick to the rotation.
    EXPECT_LT(DATA_FALRIC, 3u) << "a boss-state index, and also Falric's guid key";
    EXPECT_LT(DATA_MARWYN, 3u) << "a boss-state index, and also Marwyn's guid key";

    // The persistent vector is its own, separate key space starting at 0.
    EXPECT_EQ(PERSISTENT_DATA_INTRO, 0u);
    EXPECT_EQ(PERSISTENT_DATA_FROSTSWORN_GENERAL, 1u);
    EXPECT_EQ(PERSISTENT_DATA_LK_INTRO, 2u);
    EXPECT_EQ(PERSISTENT_DATA_BATTERED_HILT, 3u);
}

// The leashes are the instance's own #defines and are not ours to tune.
TEST(DungeonEventHallsOfReflectionTest, TheLeashesAreTheInstanceScriptsOwnNumbers)
{
    EXPECT_FLOAT_EQ(LEASH_COMBAT, 70.5f);   // MAX_DIST_FROM_CENTER_IN_COMBAT
    EXPECT_FLOAT_EQ(LEASH_RESTART, 40.0f);  // MAX_DIST_FROM_CENTER_TO_START
    EXPECT_LT(LEASH_RESTART, LEASH_COMBAT);
}

// THE ROSTER ROW THAT MAKES THE ESCAPE AN ENCOUNTER. It is the Nexus Frozen
// Commander shape and not the Pit of Saron one, and the difference matters:
// "Escaped from Arthas" has DungeonEncounter.dbc rows 843/844 and NO
// instance_encounters row, so there is no credit entry to join on AND no kill bit
// anywhere. MakeBossWithBit would be wrong; completion has to come off the
// instance's own boss-state slot.
TEST(DungeonEventHallsOfReflectionTest, TheEscapeIsARosterRowCompletedByBossState)
{
    BossRosterPatch const* patch = nullptr;
    for (BossRosterPatch const& p : BossRosterRegistry::AllPatches())
        if (p.mapId == MAP_ID)
            patch = &p;

    ASSERT_NE(patch, nullptr)
        << "map 668 has no roster patch. Without one the derived roster is Falric and Marwyn "
           "only — the escape is not an anchor, nothing walks the party down the path, and a "
           "run that killed Marwyn would report itself complete.";

    DungeonBossInfo const* lk = nullptr;
    for (DungeonBossInfo const& e : patch->add)
        if (e.entry == NPC_LICH_KING)
            lk = &e;

    ASSERT_NE(lk, nullptr) << "the Lich King (36954) is not added by the patch";
    EXPECT_EQ(lk->kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(lk->doneBossStateIndex, static_cast<int32>(DATA_LICH_KING))
        << "the escape completes through GetBossState(DATA_LICH_KING) == DONE and nothing "
           "else — there is no encounter bit for it to carry";
    EXPECT_EQ(lk->encounterIndex, 64u)
        << "MakeBoss parks encounterIndex past bit 31 when doneBossStateIndex is set, so the "
           "completed-mask check never matches a real boss's bit";
    EXPECT_EQ(lk->inheritCompletionFrom, 0u)
        << "there is nothing to inherit from: the encounter has no instance_encounters row";
    EXPECT_EQ(lk->orderOverride, ORDER_LICH_KING);

    // THE ANCHOR IS THE END OF THE PATH, NOT HIS SPAWN. Anchoring him where he
    // stands frozen (5552.77, 2262.57) would mean that on every tick the escape
    // driver is not active — before the gossip, and after a wipe — the ordinary
    // clear walks the party AT a Lich King.
    EXPECT_NEAR(lk->x, PATH_WAYPOINTS[18].x, 0.5f);
    EXPECT_NEAR(lk->y, PATH_WAYPOINTS[18].y, 0.5f);
    EXPECT_GT(std::fabs(lk->y - LK_SPAWN_Y), 500.0f)
        << "the Lich King must be anchored at WP18, 679yd down the escape path, and not at "
           "the spawn the party musters 30yd from";

    // Three objectives, each pointing at its own event.
    struct Want { uint32 seq; uint32 eventId; int32 order; };
    Want const wants[] = {
        { 1, EVENT_INTRO,   ORDER_INTRO   },
        { 2, EVENT_GENERAL, ORDER_GENERAL },
        { 3, EVENT_THRONE,  ORDER_THRONE  },
    };
    for (Want const& w : wants)
    {
        DungeonBossInfo const* obj = nullptr;
        for (DungeonBossInfo const& e : patch->add)
            if (e.entry == BossRosterRegistry::ObjectiveEntry(w.seq))
                obj = &e;
        ASSERT_NE(obj, nullptr) << "OBJ(" << w.seq << ") is missing from the patch";
        EXPECT_EQ(obj->kind, DungeonAnchorKind::Objective);
        EXPECT_EQ(obj->eventId, w.eventId);
        EXPECT_EQ(obj->orderOverride, w.order);
        EXPECT_EQ(obj->encounterIndex, 0u)
            << "an objective carries no kill-bit; it orders by orderOverride";
        EXPECT_GT(obj->arriveRadius, 0.0f) << "an objective with no arrive radius never arrives";
    }

    // The two reorders exist only to make integer room for the objectives.
    bool falric = false, marwyn = false;
    for (auto const& r : patch->reorder)
    {
        if (r.first == NPC_FALRIC) { falric = true; EXPECT_EQ(r.second, ORDER_FALRIC); }
        if (r.first == NPC_MARWYN) { marwyn = true; EXPECT_EQ(r.second, ORDER_MARWYN); }
    }
    EXPECT_TRUE(falric);
    EXPECT_TRUE(marwyn);

    EXPECT_TRUE(patch->remove.empty())
        << "nothing on this map is mis-derived; Falric and Marwyn both have real "
           "instance_encounters rows AND spawns";
    EXPECT_TRUE(patch->skipByDesign.empty());
}

// The clear order has to be a contiguous 1..6 with the intro objective FIRST —
// an objective ordered after the boss it gates never runs, and on this map the
// first objective gates every other thing in the dungeon.
TEST(DungeonEventHallsOfReflectionTest, TheClearOrderIsContiguousAndTheIntroComesFirst)
{
    EXPECT_EQ(ORDER_INTRO, 1);
    EXPECT_EQ(ORDER_FALRIC, 2);
    EXPECT_EQ(ORDER_MARWYN, 3);
    EXPECT_EQ(ORDER_GENERAL, 4);
    EXPECT_EQ(ORDER_THRONE, 5);
    EXPECT_EQ(ORDER_LICH_KING, 6);
}

TEST(DungeonEventHallsOfReflectionTest, TheIntroIsAnAnchoredGossipThenAGarrison)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_INTRO);
    ASSERT_NE(ev, nullptr) << "map 668 event 1 'Start the intro' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->stepsOwnMovement) << "hook 31 walks the tank to the leader itself";
    EXPECT_TRUE(ev->required) << "no gossip, no dungeon";
    EXPECT_FALSE(ev->repeatable) << "the intro never replays";

    ASSERT_EQ(ev->steps.size(), 2u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_HOR_INTRO_GOSSIP);

    // THE GARRISON, and its gate is the same fact the wave driver arms on.
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::MoveTo);
    EXPECT_EQ(ev->steps[1].instanceDataId, static_cast<int32>(DATA_WAVE_NUMBER));
    EXPECT_EQ(ev->steps[1].instanceDataMin, 1u);
    EXPECT_EQ(ev->steps[1].hookId, 0u)
        << "no WhileHolding hook: a gated MoveTo with none is the one hold shape that does "
           "not starve the tank's rest, and the intro is nearly four minutes of standing "
           "still that the party should spend recovering";
    EXPECT_NEAR(ev->steps[1].x, CAMP_X, 0.01f);
    EXPECT_NEAR(ev->steps[1].y, CAMP_Y, 0.01f);

    // The garrison has to outlast the FULL intro, because a party without the
    // skip quest takes 224.5 seconds of it.
    EXPECT_GT(ev->steps[1].timeoutMs, 230'000u)
        << "the Alliance intro alone is 224.5s; a shorter budget stalls a run that did not "
           "get the skip quest";
}

TEST(DungeonEventHallsOfReflectionTest, TheAltarDriverIsAConditionalInCombatDriverThatOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_WAVES);
    ASSERT_NE(ev, nullptr) << "map 668 event 2 'Hold the altar' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition));
    EXPECT_TRUE(ev->repeatable) << "a leash wipe puts the wave counter back to 0 and the "
                                   "instance restarts the event on its own; a latch would "
                                   "stop the driver re-arming";
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->ownsThePull) << "the pull's Idle branch drags a camp BACK toward the "
                                    "front door, which is 65yd from centre against a 70.5yd "
                                    "wipe line";
    EXPECT_TRUE(ev->drivesInCombat) << "the gaps between waves are five seconds long";
    EXPECT_TRUE(ev->stepsOwnMovement);
    EXPECT_TRUE(ev->required);

    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_HOR_WAVES);

    // PanelAfterBoss, never PanelBeforeBoss: panelGatesBossEntry also keys
    // HasPendingSummonEvent, which on a REPEATABLE event never latches and would
    // suppress the dynamic pull within 80yd of the boss permanently.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u);
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_FALRIC);
}

TEST(DungeonEventHallsOfReflectionTest, TheGeneralIsAnAnchoredEngageKill)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_GENERAL);
    ASSERT_NE(ev, nullptr) << "map 668 event 3 'The Frostsworn General' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->required)
        << "at_hor_shadow_throne refuses SILENTLY while PERSISTENT_DATA_FROSTSWORN_GENERAL "
           "is clear, so a run that skips him stalls in the throne room with nothing to name";
    EXPECT_FALSE(ev->stepsOwnMovement) << "the engage pipeline owns this walk-in";

    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::KillCreature);
    EXPECT_TRUE(ev->steps[0].engage);
    EXPECT_EQ(ev->steps[0].creatureEntry, NPC_FROSTSWORN_GENERAL);
}

TEST(DungeonEventHallsOfReflectionTest, TheThroneRoomGathersForgesThenWalksToThePointOfNoReturn)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_THRONE);
    ASSERT_NE(ev, nullptr) << "map 668 event 4 'The throne room' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->stepsOwnMovement);

    ASSERT_EQ(ev->steps.size(), 3u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_HOR_THRONE);
    EXPECT_EQ(ev->steps[1].kind, EventStepKind::MoveTo);
    EXPECT_NEAR(ev->steps[1].x, MUSTER_X, 0.01f);
    EXPECT_EQ(ev->steps[2].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[2].hookId, HOOK_HOR_ESCAPE_GO);

    // THE ORDER IS THE DESIGN. The gather-and-forge has to finish before the walk
    // to the leader, and the walk before the gossip, because the gossip is
    // irreversible: past it there is no drinking and no out-of-combat resurrect
    // for four to six minutes.
    EXPECT_LT(ev->steps[0].timeoutMs, ev->steps[2].timeoutMs)
        << "the point-of-no-return step needs the longer budget — it is waiting for the "
           "party to top off, which the escape gives no second chance at";
}

TEST(DungeonEventHallsOfReflectionTest, TheEscapeIsAConditionalInCombatDriverThatOwnsThePull)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_ESCAPE);
    ASSERT_NE(ev, nullptr) << "map 668 event 5 'Escape the Lich King' is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition));
    EXPECT_TRUE(ev->repeatable);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->ownsThePull);
    EXPECT_FALSE(ev->yieldsTheApproach)
        << "the off-line rejoin must stand down for the escape: its cursor points back at the throne";
    EXPECT_TRUE(ev->drivesInCombat)
        << "npc_hor_lich_kingAI SetInCombatWithZone()s every player once a second for the "
           "whole escape, so a non-combat-only rung would get exactly zero ticks";
    EXPECT_TRUE(ev->stepsOwnMovement);

    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_HOR_ESCAPE);

    EXPECT_EQ(ev->panelGatesBossEntry, 0u);
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_MARWYN);

    // The budget has to cover the last wall's real deadline (T0+355s) plus the
    // outro's ~40 seconds, with slack for a wipe.
    EXPECT_GT(ev->steps[0].timeoutMs, 400'000u);
}

TEST(DungeonEventHallsOfReflectionTest, AllFiveHooksAreRegisteredOnTheFlatIdSpace)
{
    for (uint32 id : { HOOK_HOR_INTRO_GOSSIP, HOOK_HOR_WAVES, HOOK_HOR_THRONE,
                       HOOK_HOR_ESCAPE_GO, HOOK_HOR_ESCAPE })
        EXPECT_TRUE(ObjectiveHookRegistry::Has(id))
            << "hook " << id << " is not registered; a Custom step with an unregistered "
                                "hook Blocks and stalls the run";

    // Hook ids are ONE FLAT SPACE across every dungeon; 29-30 are Pit of Saron's.
    EXPECT_EQ(HOOK_HOR_INTRO_GOSSIP, 31u);
    EXPECT_EQ(HOOK_HOR_ESCAPE, 35u);
}

// The four doors are the instance's alone. The FRONT DOOR is the one that
// matters: it is SHUT for most of the first two thirds of the run with the whole
// party deliberately on the inside of it, so a run that treats it as a corridor
// blocker auto-pauses on an encounter that is working exactly as designed.
TEST(DungeonEventHallsOfReflectionTest, EveryDoorIsScriptOnlyAndStillVisibleToNavigation)
{
    for (uint32 go : { GO_FRONT_DOOR, GO_ARTHAS_DOOR, GO_DOOR_BEFORE_THRONE, GO_ICE_WALL })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go))
            << "gameobject " << go << " left unflagged is a door a bot will try to click, "
                                      "fighting the instance script for its state";
        // DELIBERATELY NOT navigation-ignored, for the Pit of Saron reason: the
        // party never needs to path THROUGH any of these, so a run that pauses at
        // one has regressed somewhere the module should be told about.
        EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << "gameobject " << go << " must stay visible to navigation so a regression "
                                      "here is reported rather than masked";
    }
}

// ...and the FIFTH AND SIXTH door-typed gameobjects on this map, which are the
// exception to every line of the test above because neither is a door.
//
// THEY ARE A PAIR, AND THAT IS THE WHOLE POINT OF THIS TEST. Both stand on the
// Frostmourne dais within 0.03yd of each other — GO 202236 'Frostmourne Altar'
// at (5309.34, 2006.52) and GO 202302 'Frostmourne', the sword, at
// (5309.36, 2006.55) — and both wear a GAMEOBJECT_TYPE_DOOR template spawned in
// state 1. Leg A of the route begins at CenterPos, 0.13yd from that origin, so
// the corridor's first leg transits BOTH footprints the instant the route to the
// Frostsworn General is seeded, and neither can be clicked open.
//
// They fail for different reasons, which is why one row did not cover the other:
//
//   - The ALTAR is never scripted at all. instance_halls_of_reflection lists it
//     in objectData for lookups only — no HandleGameObject, no doorData row — so
//     the closed-door predicate reads it shut on every tick, forever.
//   - The SWORD is scripted, but only ever SHUT: closed on create, opened for
//     the intro cutscene, then closed again and SetPhaseMask(2)'d at the end of
//     the Lich King intro. That chain runs on the SKIPPED intro too (it is what
//     spawns Falric), so past the intro it is shut and out of the party's phase
//     for the rest of the run.
//
// Listing one and not the other changes nothing: the scan just flags whichever
// is left. That is not a hypothesis — it is the recorded history of this bug.
// Plan tp-20260907-212113-1 lost all ten runs at 3/6 bosses flagging the altar
// (GUID 0xf1100315fc000003); the altar was whitelisted; and plan
// tp-20260907-214408-1 then lost all ten runs at 3/6 bosses flagging the sword
// (GUID 0xf11003163e000002), each within a second of Marwyn dying.
TEST(DungeonEventHallsOfReflectionTest, TheFrostmourneDaisPairIsNavigationIgnoredNotScriptDoors)
{
    for (uint32 go : { GO_FROSTMOURNE_ALTAR, GO_FROSTMOURNE })
    {
        EXPECT_TRUE(DcEventDoorRegistry::IsNavigationIgnored(go))
            << "gameobject " << go << " sits on Leg A's first anchor and can never open, "
                                      "so leaving it visible to the blocking-door scan "
                                      "auto-pauses every run the moment Marwyn dies";

        // IsScriptOnly would suppress only the CLICK; the auto-pause underneath
        // it is what actually kills the run, so neither may be filed there
        // instead — the same distinction the Chromaggus portcullis row documents.
        EXPECT_FALSE(DcEventDoorRegistry::IsScriptOnly(go))
            << "gameobject " << go << " filed as script-only would leave the run dead on "
                                      "the dais with the click merely suppressed";

        // And neither may be mistaken for a clickable gate: nothing opens them,
        // so a bot entitled to click would work one until the door-blocked
        // watchdog gave up and auto-paused anyway.
        EXPECT_FALSE(DcEventDoorRegistry::IsLockFreeClickable(go))
            << "gameobject " << go << " has no open state to reach; clicking it can only "
                                      "burn the DoorBlockedTimeout budget before pausing";
    }

    // The two are distinct entries, not one constant spelled twice — the bug was
    // exactly the belief that the dais held a single door-typed gameobject.
    EXPECT_NE(GO_FROSTMOURNE_ALTAR, GO_FROSTMOURNE);
}

// THE LICH KING MUST BE UNTOUCHABLE BY EVERY MEMBER, which needs both registries
// and is the one place on this map where a missing row is a guaranteed wipe.
TEST(DungeonEventHallsOfReflectionTest, TheLichKingIsBarredByBothTargetRegistries)
{
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, NPC_LICH_KING))
        << "without this the clear's own FarTargets / RoomTrash / BlockingTrash scans "
           "propose him and walk the party at a creature that heals to 75% below 70";

    // ...and the intro Lich King, Uther and the Ice Wall Targets, all of which
    // stand on ground the party occupies for minutes at a time.
    for (uint32 entry : { NPC_LICH_KING_INTRO, NPC_UTHER, NPC_ICE_WALL_TARGET })
        EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(MAP_ID, entry));

    // The exclusion registry is the half that reaches the STOCK combat engine's
    // own target selection, which is what actually points the party's damage.
    // AttackersValue::IsPossibleTarget accepts 36954 without reservation — he has
    // unit_flags 0 and no immunities — so a never-target row alone changes nothing
    // about what the assist triggers do.
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID))
        << "map 668 has no target-exclusion row, so stock TankAssistTrigger and "
           "NotDpsTargetActiveTrigger acquire the Lich King within about a second of the "
           "escape gossip — and one bot holding him as a victim freezes every MayDrive rung "
           "in the party while he walks at them at 1.4 yd/s";
}

// The ring is a PLACEMENT keep-out and deliberately NOT an active-vacate row: the
// generic vacate retreats radially, and on this encounter away-from-him is the
// one direction that makes things worse. The forward answer is a relevance rung
// of its own.
TEST(DungeonEventHallsOfReflectionTest, RemorselessWinterIsAKeepOutAndNotARadialVacate)
{
    DcHazardEmitter const* ring = DcHazardRegistry::Find(MAP_ID, NPC_LICH_KING);
    ASSERT_NE(ring, nullptr) << "Remorseless Winter is not registered at all";
    EXPECT_GE(ring->radius, 10.0f)
        << "the pulse is 10yd; a keep-out inside it does not keep anything out";
    EXPECT_FLOAT_EQ(ring->vacateRadius, 0.0f)
        << "a vacateRadius here would drive DungeonClearHazardVacateAction, whose retreat is "
           "RADIAL — and for a bot already behind him, away-from-him is further behind, into "
           "a 10 000-damage zap whose knockback makes the next check worse";

    // ...and the forward answer outranks the radial one it replaces.
    EXPECT_GT(DcRel::HorStayAhead, DcRel::HazardVacate);
}

TEST(DungeonEventHallsOfReflectionTest, MarwynsWellOfCorruptionIsARegisteredGroundPool)
{
    DcGroundHazard const* well = DcHazardRegistry::FindGround(MAP_ID, SPELL_WELL_OF_CORRUPTION);
    ASSERT_NE(well, nullptr)
        << "Well of Corruption (72362) drops a 3yd persistent area aura under a random party "
           "member for 8 seconds and applies +30% shadow taken in a fight whose every other "
           "ability is shadow";
    EXPECT_GT(well->vacateRadius, 0.0f) << "a pool cannot be fought, so it must be left";
    EXPECT_GE(well->radius, well->vacateRadius)
        << "the placement keep-out must be at least the pulse it describes";

    // Kept tight on purpose: the pool lands under a party that is pinned to the
    // altar by a 70.5yd leash and cannot simply relocate.
    EXPECT_LT(well->radius, 10.0f);
}

// The General evades if dragged 30yd from home, and an evade despawns his five
// Spiritual Reflections with him — on an encounter whose death is the ONLY thing
// that arms the throne-room areatrigger.
TEST(DungeonEventHallsOfReflectionTest, TheGeneralsCorridorIsAFightInPlaceZone)
{
    EXPECT_TRUE(FightInPlaceRegistry::IsNoPullZone(MAP_ID, GENERAL_HOME_X, GENERAL_HOME_Y));
    EXPECT_TRUE(FightInPlaceRegistry::IsNoPullZone(MAP_ID, GENERAL_X, GENERAL_Y))
        << "the objective anchor the party forms up on must be inside the box, or the pull "
           "could drag him back to it and out of his own evade radius";

    // ...and nothing else on the map is inside it.
    EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MAP_ID, CENTER_X, CENTER_Y))
        << "the altar must not be a no-pull zone; there is nothing to pull there, but the "
           "box would also cover ground three encounters away";
    EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MAP_ID, THRONE_X, THRONE_Y));
    EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MAP_ID, LEADER_ESCAPE_X, LEADER_ESCAPE_Y));
}

// StopIndexNear turns the leader's position into the escape's state variable —
// currentWall is private to the boss AI and nothing exposes it. Both the driver
// and the per-follower stay-ahead action read the same function, because a
// disagreement would hold the tank and its followers on different ground with a
// Lich King between them.
TEST(DungeonEventHallsOfReflectionTest, StopIndexNearResolvesEachWaypointAndEveryGapForward)
{
    // Standing exactly on each stop.
    for (uint8 i = 0; i < STOP_COUNT; ++i)
    {
        HorPoint const& p = PATH_WAYPOINTS[WP_STOP[i]];
        EXPECT_EQ(StopIndexNear(p.x, p.y), i) << "stop " << int(i) << " does not resolve to itself";
    }

    // Standing between two stops resolves to the one she is running TO, which is
    // where the party should be walking as well. The stops are 100-176yd apart,
    // so the mid-path waypoints fall well outside LEADER_STOP_SNAP and it is the
    // (x + y) scan that answers — which is the branch that has to be right,
    // because it is the one the leader is in for every second she is moving.
    //
    //   WP7  lies between stop 1 (WP5)  and stop 2 (WP8)  -> 2
    //   WP12 lies between stop 3 (WP10) and stop 4 (WP14) -> 4
    //   WP16 lies between stop 4 (WP14) and the end (WP18) -> 5
    EXPECT_EQ(StopIndexNear(PATH_WAYPOINTS[7].x, PATH_WAYPOINTS[7].y), 2);
    EXPECT_EQ(StopIndexNear(PATH_WAYPOINTS[12].x, PATH_WAYPOINTS[12].y), 4);
    EXPECT_EQ(StopIndexNear(PATH_WAYPOINTS[16].x, PATH_WAYPOINTS[16].y), 5);

    // Every stand point resolves to its own stop's index — the party is holding
    // ground five yards ahead of her, not a wall further on.
    for (uint8 k = 0; k < 4; ++k)
        EXPECT_EQ(StopIndexNear(STAND_POINTS[k].x, STAND_POINTS[k].y), k + 1)
            << "stand point " << int(k + 1) << " resolves to the wrong stop";
}

TEST(DungeonEventHallsOfReflectionTest, StandPointForCoversEveryStopAndClampsAtTheEnd)
{
    for (uint8 k = 1; k <= 4; ++k)
    {
        HorPoint const p = StandPointFor(k);
        EXPECT_FLOAT_EQ(p.x, STAND_POINTS[k - 1].x);
        EXPECT_FLOAT_EQ(p.y, STAND_POINTS[k - 1].y);
    }
    HorPoint const end = StandPointFor(5);
    EXPECT_FLOAT_EQ(end.x, PATH_WAYPOINTS[18].x);
    EXPECT_FLOAT_EQ(end.y, PATH_WAYPOINTS[18].y);
}

// The path table is the core's PathWaypoints verbatim, and WP_STOP indexes it.
// Getting either wrong would put the party's stand points on ground the encounter
// never visits.
TEST(DungeonEventHallsOfReflectionTest, ThePathTableMatchesTheCoresOwnWaypoints)
{
    EXPECT_EQ(PATH_WP_COUNT, 19u);
    EXPECT_EQ(STOP_COUNT, 6u);
    EXPECT_EQ(WP_STOP[0], 0);
    EXPECT_EQ(WP_STOP[1], 5);
    EXPECT_EQ(WP_STOP[2], 8);
    EXPECT_EQ(WP_STOP[3], 10);
    EXPECT_EQ(WP_STOP[4], 14);
    EXPECT_EQ(WP_STOP[5], 18);

    // Spot-checks against halls_of_reflection.h.
    EXPECT_NEAR(PATH_WAYPOINTS[0].x, 5588.055664f, 0.001f);
    EXPECT_NEAR(PATH_WAYPOINTS[5].y, 2103.950928f, 0.001f);
    EXPECT_NEAR(PATH_WAYPOINTS[18].z, 784.301697f, 0.001f);

    // The path runs -x AND -y throughout, which is what makes the encounter's own
    // "is this player behind me" test a single scalar.
    for (uint32 i = 3; i + 1 < PATH_WP_COUNT; ++i)
        EXPECT_LT(PATH_WAYPOINTS[i + 1].x + PATH_WAYPOINTS[i + 1].y,
                  PATH_WAYPOINTS[i].x + PATH_WAYPOINTS[i].y)
            << "waypoint " << (i + 1) << " moves backward on the (x + y) scalar";
}

// The escape gossip's mana floor is not a nicety: past it there is no drinking
// for four to six minutes, and the healer has four wall fights to cover.
TEST(DungeonEventHallsOfReflectionTest, ThePointOfNoReturnDemandsMoreThanTheOrdinaryGather)
{
    EXPECT_GE(ESCAPE_MANA_PCT, 75.0f);
    EXPECT_LE(ESCAPE_MANA_PCT, 100.0f);
    // The throne-room gather is the ordinary 3-of-4; the gossip after it is
    // 5-of-5, enforced in hook 34 rather than by a constant. This pins the
    // constant that the LOOSER of the two uses, so a future edit that tightened
    // the wrong one is visible.
    EXPECT_FLOAT_EQ(GATHER_QUORUM, 0.75f);
}
